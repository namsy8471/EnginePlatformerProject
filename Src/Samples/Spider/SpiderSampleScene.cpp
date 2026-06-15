#include "SpiderSampleScene.h"

#include "Rendering/Resources/MaterialTextureSystem.h"
#include "Scene/SceneLoader.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <windows.h>

namespace Samples::Spider
{
	namespace
	{
		void LogEngineTrace(std::string_view message)
		{
			constexpr std::string_view prefix = "[Engine][TRACE] ";
			constexpr std::string_view suffix = "\n";

			std::string buffer;
			buffer.reserve(prefix.size() + message.size() + suffix.size());
			buffer.append(prefix);
			buffer.append(message);
			buffer.append(suffix);
			OutputDebugStringA(buffer.c_str());
		}

		void ApplyVertexTint(Asset::StaticMeshAsset& mesh, const DirectX::XMFLOAT4& tint)
		{
			for (auto& vertex : mesh.Vertices)
			{
				vertex.Color = tint;
			}
			for (auto& vertex : mesh.BindPoseVertices)
			{
				vertex.Color = tint;
			}
		}
	}

	bool Load(Scene& scene, SceneRenderState& renderState, Camera& camera, LoadResult& result)
	{
		renderState.Reset();
		scene.ResetSelection();
		result = {};

		const std::filesystem::path spiderDirectory = "Assets/Models/Spider";
		if (!std::filesystem::exists(spiderDirectory))
		{
			LogEngineTrace("Assets/Models/Spider folder was not found.");
			return false;
		}

		const SceneLoader::EntityLoadResult loadResult = SceneLoader::LoadFirstModelEntity(scene, "Spider", spiderDirectory);
		const EntityId spiderEntity = loadResult.Entity;
		const std::filesystem::path& selectedModelPath = loadResult.SelectedPath;
		if (spiderEntity != InvalidEntityId)
		{
			result.SpiderEntity = spiderEntity;
			renderState.RenderEntities.push_back(spiderEntity);
			scene.SetPrimaryRenderableEntity(spiderEntity);
			LogEngineTrace(loadResult.IsAnimated ? "Auto-routed Spider asset to animated loader." : "Auto-routed Spider asset to static loader.");
		}

		Asset::StaticMeshAsset* spiderMesh = scene.GetMeshAsset(spiderEntity);
		if (selectedModelPath.empty() || !spiderMesh || spiderMesh->Vertices.empty() || spiderMesh->Indices.empty())
		{
			LogEngineTrace("Spider sample scene failed to load a renderable model.");
			return false;
		}

		DirectX::XMFLOAT2 minUv(
			(std::numeric_limits<float>::max)(),
			(std::numeric_limits<float>::max)());
		DirectX::XMFLOAT2 maxUv(
			(std::numeric_limits<float>::lowest)(),
			(std::numeric_limits<float>::lowest)());
		uint32_t outOfRangeUvCount = 0;
		for (const auto& vertex : spiderMesh->Vertices)
		{
			minUv.x = (std::min)(minUv.x, vertex.TexCoord.x);
			minUv.y = (std::min)(minUv.y, vertex.TexCoord.y);
			maxUv.x = (std::max)(maxUv.x, vertex.TexCoord.x);
			maxUv.y = (std::max)(maxUv.y, vertex.TexCoord.y);
			if (vertex.TexCoord.x < 0.0f || vertex.TexCoord.x > 1.0f || vertex.TexCoord.y < 0.0f || vertex.TexCoord.y > 1.0f)
			{
				++outOfRangeUvCount;
			}
		}

		std::string uvLogMessage = "UV diagnostics - Min(";
		uvLogMessage.append(std::to_string(minUv.x));
		uvLogMessage.append(", ");
		uvLogMessage.append(std::to_string(minUv.y));
		uvLogMessage.append(") Max(");
		uvLogMessage.append(std::to_string(maxUv.x));
		uvLogMessage.append(", ");
		uvLogMessage.append(std::to_string(maxUv.y));
		uvLogMessage.append(") OutOfRangeVertexCount=");
		uvLogMessage.append(std::to_string(outOfRangeUvCount));
		LogEngineTrace(uvLogMessage);

		if (!Rendering::MaterialTextureSystem::LoadCpuMaterialTextures(scene, renderState, spiderEntity, LogEngineTrace))
		{
			return false;
		}

		if (auto* spiderTransform = scene.GetTransformComponent(spiderEntity))
		{
			spiderTransform->SetLocalTransform(Math::Transform::Identity());
			spiderTransform->UpdateWorld();
		}

		const auto* sourceMaterialTextures = scene.GetMaterialTextures(spiderEntity);
		const std::array<DirectX::XMFLOAT4, 10> tintPalette = {
			DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f },
			DirectX::XMFLOAT4{ 1.0f, 0.6f, 0.6f, 1.0f },
			DirectX::XMFLOAT4{ 0.6f, 1.0f, 0.6f, 1.0f },
			DirectX::XMFLOAT4{ 0.6f, 0.8f, 1.0f, 1.0f },
			DirectX::XMFLOAT4{ 1.0f, 0.8f, 0.4f, 1.0f },
			DirectX::XMFLOAT4{ 0.9f, 0.5f, 1.0f, 1.0f },
			DirectX::XMFLOAT4{ 0.5f, 1.0f, 1.0f, 1.0f },
			DirectX::XMFLOAT4{ 1.0f, 0.7f, 0.9f, 1.0f },
			DirectX::XMFLOAT4{ 0.8f, 1.0f, 0.7f, 1.0f },
			DirectX::XMFLOAT4{ 1.0f, 0.9f, 0.7f, 1.0f }
		};

		ApplyVertexTint(*spiderMesh, tintPalette[0]);
		const std::vector<bool> baseMaterialTransparency = renderState.PrimaryMaterialTransparency;

		auto spawnVariant = [&](int variantIndex, bool glassVariant)
		{
			std::string entityName = glassVariant ? "Glass_" : "Spider_";
			entityName.append(std::to_string(variantIndex + 1));

			const EntityId entity = scene.CreateEntity(entityName);
			MeshComponent& meshComponent = scene.EnsureMeshComponent(entity);
			meshComponent.Asset = std::make_unique<Asset::StaticMeshAsset>(*spiderMesh);
			if (sourceMaterialTextures)
			{
				meshComponent.MaterialTextures = *sourceMaterialTextures;
			}
			renderState.EntityMaterialTransparency[entity] = baseMaterialTransparency;

			const size_t colorIndex = static_cast<size_t>(variantIndex) % tintPalette.size();
			DirectX::XMFLOAT4 tint = tintPalette[colorIndex];
			if (glassVariant)
			{
				tint.w = 0.35f;
				renderState.TransparentEntities.insert(entity);
			}
			ApplyVertexTint(*meshComponent.Asset, tint);

			TransformComponent& transform = scene.EnsureTransformComponent(entity);
			const float x = static_cast<float>((variantIndex % 5) - 2) * 1.8f;
			const float z = glassVariant
				? 3.0f + static_cast<float>(variantIndex / 5) * 2.2f
				: -2.0f - static_cast<float>(variantIndex / 5) * 2.2f;
			transform.LocalTransform = Math::Transform(
				{ x, 0.0f, z },
				Math::IdentityQuaternion(),
				{ 0.4f, 0.4f, 0.4f });
			transform.UpdateWorld();

			if (const BoundsComponent* sourceBounds = scene.GetBoundsComponent(spiderEntity))
			{
				BoundsComponent& bounds = scene.EnsureBoundsComponent(entity);
				bounds = *sourceBounds;
			}

			renderState.RenderEntities.push_back(entity);
		};

		for (int index = 1; index < 10; ++index)
		{
			spawnVariant(index, false);
		}

		for (int index = 0; index < 10; ++index)
		{
			spawnVariant(index, true);
		}

		DirectX::XMFLOAT3 minBounds(
			(std::numeric_limits<float>::max)(),
			(std::numeric_limits<float>::max)(),
			(std::numeric_limits<float>::max)());
		DirectX::XMFLOAT3 maxBounds(
			(std::numeric_limits<float>::lowest)(),
			(std::numeric_limits<float>::lowest)(),
			(std::numeric_limits<float>::lowest)());

		for (const auto& vertex : spiderMesh->Vertices)
		{
			minBounds.x = (std::min)(minBounds.x, vertex.Position.x);
			minBounds.y = (std::min)(minBounds.y, vertex.Position.y);
			minBounds.z = (std::min)(minBounds.z, vertex.Position.z);
			maxBounds.x = (std::max)(maxBounds.x, vertex.Position.x);
			maxBounds.y = (std::max)(maxBounds.y, vertex.Position.y);
			maxBounds.z = (std::max)(maxBounds.z, vertex.Position.z);
		}

		if (auto* spiderBounds = scene.GetBoundsComponent(spiderEntity))
		{
			spiderBounds->LocalMin = minBounds;
			spiderBounds->LocalMax = maxBounds;
		}

		const DirectX::XMFLOAT3 center = {
			(minBounds.x + maxBounds.x) * 0.5f,
			(minBounds.y + maxBounds.y) * 0.5f,
			(minBounds.z + maxBounds.z) * 0.5f
		};
		const float extentX = maxBounds.x - minBounds.x;
		const float extentY = maxBounds.y - minBounds.y;
		const float extentZ = maxBounds.z - minBounds.z;
		const float maxExtent = (std::max)(extentX, (std::max)(extentY, extentZ));
		const float cameraDistance = (std::max)(maxExtent * 2.5f, 3.0f);
		camera.LookAt(
			{ center.x, center.y + maxExtent * 0.35f, center.z - cameraDistance },
			center,
			{ 0.0f, 1.0f, 0.0f });

		std::string logMessage = "Loaded Spider static mesh: ";
		logMessage.append(selectedModelPath.string());
		LogEngineTrace(logMessage);
		return true;
	}
}
