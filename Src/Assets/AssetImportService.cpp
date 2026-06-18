#include "AssetImportService.h"

#include "Assets/AssimpModelLoader.h"
#include "Assets/AssetFileSystem.h"
#include "Assets/AssetImportSettings.h"
#include "Rendering/Resources/MaterialTextureSystem.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <string_view>
#include <system_error>

namespace Asset
{
	namespace
	{
		[[nodiscard]] std::filesystem::path NormalizePath(const std::filesystem::path& path)
		{
			std::error_code errorCode;
			const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, errorCode);
			return (errorCode ? path : canonicalPath).lexically_normal();
		}

		void ComputeBounds(const StaticMeshAsset& mesh, DirectX::XMFLOAT3& localMin, DirectX::XMFLOAT3& localMax)
		{
			localMin = {
				(std::numeric_limits<float>::max)(),
				(std::numeric_limits<float>::max)(),
				(std::numeric_limits<float>::max)()
			};
			localMax = {
				(std::numeric_limits<float>::lowest)(),
				(std::numeric_limits<float>::lowest)(),
				(std::numeric_limits<float>::lowest)()
			};

			for (const auto& vertex : mesh.Vertices)
			{
				localMin.x = (std::min)(localMin.x, vertex.Position.x);
				localMin.y = (std::min)(localMin.y, vertex.Position.y);
				localMin.z = (std::min)(localMin.z, vertex.Position.z);
				localMax.x = (std::max)(localMax.x, vertex.Position.x);
				localMax.y = (std::max)(localMax.y, vertex.Position.y);
				localMax.z = (std::max)(localMax.z, vertex.Position.z);
			}

			if (mesh.Vertices.empty())
			{
				localMin = { -0.5f, -0.5f, -0.5f };
				localMax = { 0.5f, 0.5f, 0.5f };
			}
		}

		[[nodiscard]] constexpr const char* BoolText(bool value) noexcept
		{
			return value ? "true" : "false";
		}

		void AppendInspectionDiagnostics(
			std::vector<std::string>& diagnostics,
			const ModelInspectionSummary& inspection,
			std::string_view selectedLoader)
		{
			diagnostics.push_back(std::format("Import loader path: {}", selectedLoader));
			diagnostics.push_back(std::format(
				"Assimp scene: hasScene={}, hasRootNode={}, incomplete={}",
				BoolText(inspection.HasScene),
				BoolText(inspection.HasRootNode),
				BoolText(inspection.IsIncomplete)));
			diagnostics.push_back(std::format(
				"Assimp counts: meshes={}, renderableMeshes={}, vertices={}, faces={}, indices={}, materials={}, animations={}",
				inspection.MeshCount,
				inspection.RenderableMeshCount,
				inspection.VertexCount,
				inspection.FaceCount,
				inspection.IndexCount,
				inspection.MaterialCount,
				inspection.AnimationCount));

			if (!inspection.AssimpError.empty())
			{
				diagnostics.push_back(std::format("Assimp message: {}", inspection.AssimpError));
			}
		}

		void AppendGeometryFailureDiagnostics(
			std::vector<std::string>& diagnostics,
			const ModelInspectionSummary& inspection,
			const StaticMeshAsset* mesh,
			bool selectedAnimatedLoader)
		{
			if (mesh)
			{
				diagnostics.push_back(std::format(
					"Built mesh: vertices={}, indices={}, submeshes={}, bones={}, clips={}",
					mesh->Vertices.size(),
					mesh->Indices.size(),
					mesh->Submeshes.size(),
					mesh->Bones.size(),
					mesh->Animations.size()));
			}

			if (!inspection.HasScene)
			{
				diagnostics.push_back("Likely cause: Assimp could not parse this model file.");
				return;
			}

			if (!inspection.HasRootNode || inspection.IsIncomplete)
			{
				diagnostics.push_back("Likely cause: imported scene is incomplete or has no root node.");
				return;
			}

			if (inspection.AnimationCount > 0 && inspection.RenderableMeshCount == 0)
			{
				diagnostics.push_back("Likely cause: animation-only FBX. It contains animation data but no renderable triangle mesh.");
				return;
			}

			if (inspection.MeshCount > 0 && inspection.RenderableMeshCount == 0)
			{
				diagnostics.push_back("Likely cause: helper, collision, point, or line geometry only. The renderer needs triangle mesh geometry.");
				return;
			}

			if (inspection.MeshCount == 0)
			{
				diagnostics.push_back("Likely cause: this file contains no mesh entries.");
				return;
			}

			diagnostics.push_back(selectedAnimatedLoader
				? "Likely cause: animated loader could not build skinned render geometry from this FBX."
				: "Likely cause: static loader could not build render geometry from this model.");
		}

		void AppendMaterialDiagnostics(std::vector<std::string>& diagnostics, const StaticMeshAsset& mesh)
		{
			diagnostics.push_back(std::format("Material count: {}", mesh.Materials.size()));
			for (size_t materialIndex = 0; materialIndex < mesh.Materials.size(); ++materialIndex)
			{
				const StaticMeshMaterial& material = mesh.Materials[materialIndex];
				diagnostics.push_back(std::format(
					"Material[{}] '{}' shadingModel={}",
					materialIndex,
					material.Name.empty() ? "<unnamed>" : material.Name,
					MaterialShadingModelName(material.ShadingModel)));

				for (size_t slotIndex = 0; slotIndex < kMaterialTextureSlotCount; ++slotIndex)
				{
					const auto slot = static_cast<MaterialTextureSlot>(slotIndex);
					const MaterialTextureBinding& binding = material.TextureBindings[slotIndex];
					if (!binding.HasSource())
					{
						continue;
					}

					diagnostics.push_back(std::format(
						"  {}: {}{}",
						MaterialTextureSlotName(slot),
						binding.Path.empty() ? "<embedded>" : binding.Path.string(),
						binding.IsOverride ? " (override)" : ""));
				}
			}
		}

		[[nodiscard]] float DegreesToRadians(float degrees) noexcept
		{
			return degrees * DirectX::XM_PI / 180.0f;
		}

		void ApplyImportTransform(StaticMeshAsset& mesh, const ModelImportSettings& settings, std::vector<std::string>& diagnostics)
		{
			const bool hasScale = std::isfinite(settings.Scale) && std::fabs(settings.Scale - 1.0f) > 1.0e-5f;
			const bool hasRotation =
				std::fabs(settings.RotationOffset.x) > 1.0e-5f ||
				std::fabs(settings.RotationOffset.y) > 1.0e-5f ||
				std::fabs(settings.RotationOffset.z) > 1.0e-5f;
			if (!hasScale && !hasRotation)
			{
				return;
			}

			if (mesh.IsAnimated)
			{
				diagnostics.push_back("Import settings: scale/rotationOffset skipped for animated mesh to preserve CPU skinning bind pose.");
				return;
			}

			const float scale = std::isfinite(settings.Scale) && settings.Scale > 0.0f ? settings.Scale : 1.0f;
			const DirectX::XMMATRIX transform =
				DirectX::XMMatrixScaling(scale, scale, scale) *
				DirectX::XMMatrixRotationRollPitchYaw(
					DegreesToRadians(settings.RotationOffset.x),
					DegreesToRadians(settings.RotationOffset.y),
					DegreesToRadians(settings.RotationOffset.z));
			const DirectX::XMMATRIX normalTransform = DirectX::XMMatrixRotationRollPitchYaw(
				DegreesToRadians(settings.RotationOffset.x),
				DegreesToRadians(settings.RotationOffset.y),
				DegreesToRadians(settings.RotationOffset.z));

			for (StaticMeshVertex& vertex : mesh.Vertices)
			{
				DirectX::XMStoreFloat3(
					&vertex.Position,
					DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&vertex.Position), transform));
				DirectX::XMStoreFloat3(
					&vertex.Normal,
					DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&vertex.Normal), normalTransform)));
				DirectX::XMStoreFloat3(
					&vertex.Tangent,
					DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&vertex.Tangent), normalTransform)));
			}
			mesh.BindPoseVertices = mesh.Vertices;
			diagnostics.push_back(std::format(
				"Import settings applied: scale={:.4f}, rotationOffsetDegrees=({:.2f}, {:.2f}, {:.2f})",
				scale,
				settings.RotationOffset.x,
				settings.RotationOffset.y,
				settings.RotationOffset.z));
		}

		void ApplyMaterialImportSettings(StaticMeshAsset& mesh, const ModelImportSettings& settings, std::vector<std::string>& diagnostics)
		{
			if (!settings.ImportMaterials)
			{
				StaticMeshMaterial defaultMaterial;
				defaultMaterial.Name = "Default Imported Material";
				defaultMaterial.ShadingModel = MaterialShadingModel::Phong;
				defaultMaterial.DiffuseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
				defaultMaterial.UseVertexColor = false;
				mesh.Materials.clear();
				mesh.Materials.push_back(std::move(defaultMaterial));
				for (StaticMeshSubmesh& submesh : mesh.Submeshes)
				{
					submesh.MaterialIndex = 0;
				}
				diagnostics.push_back("Import settings applied: importMaterials=false, replaced imported materials with one default Phong material.");
				return;
			}

			for (StaticMeshMaterial& material : mesh.Materials)
			{
				material.NormalYFlip = settings.NormalYFlip;
			}
			if (settings.NormalYFlip)
			{
				diagnostics.push_back("Import settings applied: normalYFlip=true for imported materials.");
			}
		}

		void ApplyAnimationImportSettings(StaticMeshAsset& mesh, const ModelImportSettings& settings, std::vector<std::string>& diagnostics)
		{
			if (settings.ImportAnimations)
			{
				return;
			}

			mesh.IsAnimated = false;
			mesh.AnimationCount = 0;
			mesh.BoneCount = 0;
			mesh.Bones.clear();
			mesh.BoneIndices.clear();
			mesh.Animations.clear();
			for (StaticMeshVertex& vertex : mesh.Vertices)
			{
				vertex.BoneIndices = { 0, 0, 0, 0 };
				vertex.BoneWeights = { 0.0f, 0.0f, 0.0f, 0.0f };
			}
			mesh.BindPoseVertices = mesh.Vertices;
			diagnostics.push_back("Import settings applied: importAnimations=false, imported as static mesh.");
		}
	}

	AssetImportService::AssetImportService()
	{
		const uint32_t workerCount = (std::max)(1u, (std::min)(4u, std::thread::hardware_concurrency()));
		m_Workers.reserve(workerCount);
		for (uint32_t workerIndex = 0; workerIndex < workerCount; ++workerIndex)
		{
			m_Workers.emplace_back([this](std::stop_token stopToken)
				{
					WorkerLoop(stopToken);
				});
		}
	}

	AssetImportService::~AssetImportService()
	{
		Shutdown();
	}

	void AssetImportService::Shutdown()
	{
		for (std::jthread& worker : m_Workers)
		{
			worker.request_stop();
		}
		m_Condition.notify_all();
		for (std::jthread& worker : m_Workers)
		{
			if (worker.joinable())
			{
				worker.join();
			}
		}
		m_Workers.clear();
	}

	void AssetImportService::Queue(AssetImportRequest request)
	{
		request.SourcePath = NormalizePath(request.SourcePath);
		{
			std::scoped_lock lock(m_Mutex);
			m_PendingRequests.push(std::move(request));
		}
		m_Condition.notify_one();
	}

	bool AssetImportService::TryPopCompleted(AssetImportResult& result)
	{
		std::scoped_lock lock(m_Mutex);
		if (m_CompletedResults.empty())
		{
			return false;
		}

		result = std::move(m_CompletedResults.front());
		m_CompletedResults.pop();
		return true;
	}

	bool AssetImportService::HasPendingWork() const noexcept
	{
		return m_ActiveWorkerCount.load(std::memory_order_acquire) > 0;
	}

	void AssetImportService::WorkerLoop(std::stop_token stopToken)
	{
		while (!stopToken.stop_requested())
		{
			AssetImportRequest request;
			{
				std::unique_lock lock(m_Mutex);
				m_Condition.wait(lock, stopToken, [this]()
					{
						return !m_PendingRequests.empty();
					});

				if (stopToken.stop_requested())
				{
					return;
				}

				request = std::move(m_PendingRequests.front());
				m_PendingRequests.pop();
			}

			m_ActiveWorkerCount.fetch_add(1, std::memory_order_acq_rel);
			AssetImportResult result = ImportModel(request);
			m_ActiveWorkerCount.fetch_sub(1, std::memory_order_acq_rel);

			{
				std::scoped_lock lock(m_Mutex);
				m_CompletedResults.push(std::move(result));
			}
		}
	}

	AssetImportResult AssetImportService::ImportModel(const AssetImportRequest& request)
	{
		AssetImportResult result;
		result.SourcePath = NormalizePath(request.SourcePath);
		result.Generation = request.Generation;
		result.SceneGeneration = request.SceneGeneration;
		result.IsReload = request.IsReload;
		result.Placement = request.Placement;
		result.Restore = request.Restore;

		if (!IsModelAssetPath(result.SourcePath))
		{
			result.ErrorMessage = std::format("Unsupported model extension: {}", result.SourcePath.string());
			return result;
		}

		std::error_code errorCode;
		if (!std::filesystem::is_regular_file(result.SourcePath, errorCode))
		{
			result.ErrorMessage = std::format("Model file was not found: {}", result.SourcePath.string());
			return result;
		}

		AssetImportSettings importSettings;
		importSettings.SourcePath = result.SourcePath;
		const std::filesystem::path importSettingsPath = AssetImportSettingsService::GetSettingsPathForAsset(result.SourcePath);
		const bool hasImportSettingsFile = std::filesystem::exists(importSettingsPath);
		const AssetImportSettingsResult settingsResult = AssetImportSettingsService::LoadOrDefault(result.SourcePath);
		if (settingsResult.Success)
		{
			importSettings = settingsResult.Settings;
			result.Diagnostics.push_back(std::format(
				"Import settings {}: {}",
				hasImportSettingsFile ? "loaded" : "using defaults",
				importSettingsPath.string()));
		}
		else
		{
			result.Diagnostics.push_back(std::format("Import settings unavailable, using defaults: {}", settingsResult.ErrorMessage));
		}
		result.GenerateColliders = importSettings.Model.GenerateColliders;

		AssimpModelLoader loader;
		const ModelInspectionSummary inspection = loader.InspectModel(result.SourcePath.string());
		result.IsAnimated = importSettings.Model.ImportAnimations && (inspection.HasScene ? inspection.HasAnimations : loader.HasAnimation(result.SourcePath.string()));
		AppendInspectionDiagnostics(result.Diagnostics, inspection, result.IsAnimated ? "animated" : "static");
		result.Diagnostics.push_back(std::format(
			"Import settings model: scale={:.4f}, importMaterials={}, importAnimations={}, generateColliders={}, generateTangents={}, normalYFlip={}, rotationOffsetDegrees=({:.2f}, {:.2f}, {:.2f})",
			importSettings.Model.Scale,
			BoolText(importSettings.Model.ImportMaterials),
			BoolText(importSettings.Model.ImportAnimations),
			BoolText(importSettings.Model.GenerateColliders),
			BoolText(importSettings.Model.GenerateTangents),
			BoolText(importSettings.Model.NormalYFlip),
			importSettings.Model.RotationOffset.x,
			importSettings.Model.RotationOffset.y,
			importSettings.Model.RotationOffset.z));

		const AssimpLoadOptions loadOptions{
			.GenerateTangents = importSettings.Model.GenerateTangents,
			.AllowAnimatedSceneAsStatic = !importSettings.Model.ImportAnimations
		};
		result.Mesh = result.IsAnimated
			? loader.LoadAnimatedMesh(result.SourcePath.string(), loadOptions)
			: loader.LoadStaticMesh(result.SourcePath.string(), loadOptions);

		if (!result.Mesh || result.Mesh->Vertices.empty() || result.Mesh->Indices.empty())
		{
			AppendGeometryFailureDiagnostics(result.Diagnostics, inspection, result.Mesh.get(), result.IsAnimated);
			result.Mesh.reset();
			result.ErrorMessage = std::format("Assimp failed to import renderable geometry: {}", result.SourcePath.string());
			return result;
		}

		ApplyAnimationImportSettings(*result.Mesh, importSettings.Model, result.Diagnostics);
		ApplyImportTransform(*result.Mesh, importSettings.Model, result.Diagnostics);
		ApplyMaterialImportSettings(*result.Mesh, importSettings.Model, result.Diagnostics);
		ComputeBounds(*result.Mesh, result.LocalMin, result.LocalMax);
		if (!Rendering::MaterialTextureSystem::LoadCpuMaterialTextures(
			*result.Mesh,
			result.MaterialTextures,
			&result.MaterialTransparency,
			[&result](std::string_view message)
			{
				result.Diagnostics.emplace_back(message);
			}))
		{
			result.ErrorMessage = std::format("Material texture decode failed: {}", result.SourcePath.string());
			result.Diagnostics.push_back("Likely cause: one or more material texture slots failed CPU decode.");
			result.Mesh.reset();
			return result;
		}
		AppendMaterialDiagnostics(result.Diagnostics, *result.Mesh);

		result.Success = true;
		return result;
	}
}
