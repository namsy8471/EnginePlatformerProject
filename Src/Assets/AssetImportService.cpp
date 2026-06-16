#include "AssetImportService.h"

#include "Assets/AssimpModelLoader.h"
#include "Assets/AssetFileSystem.h"
#include "Rendering/Resources/MaterialTextureSystem.h"

#include <algorithm>
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

		AssimpModelLoader loader;
		const ModelInspectionSummary inspection = loader.InspectModel(result.SourcePath.string());
		result.IsAnimated = inspection.HasScene ? inspection.HasAnimations : loader.HasAnimation(result.SourcePath.string());
		AppendInspectionDiagnostics(result.Diagnostics, inspection, result.IsAnimated ? "animated" : "static");

		result.Mesh = result.IsAnimated
			? loader.LoadAnimatedMesh(result.SourcePath.string())
			: loader.LoadStaticMesh(result.SourcePath.string());

		if (!result.Mesh || result.Mesh->Vertices.empty() || result.Mesh->Indices.empty())
		{
			AppendGeometryFailureDiagnostics(result.Diagnostics, inspection, result.Mesh.get(), result.IsAnimated);
			result.Mesh.reset();
			result.ErrorMessage = std::format("Assimp failed to import renderable geometry: {}", result.SourcePath.string());
			return result;
		}

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
