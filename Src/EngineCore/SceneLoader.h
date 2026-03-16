#pragma once

#include "Scene.h"
#include "Asset/AssimpModelLoader.h"

#include <filesystem>
#include <algorithm>
#include <string_view>
#include <vector>

namespace SceneLoader
{
	struct EntityLoadResult
	{
		EntityId Entity = InvalidEntityId;
		std::filesystem::path SelectedPath;
		bool IsAnimated = false;
	};

	[[nodiscard]] inline EntityLoadResult LoadFirstModelEntity(Scene& scene, std::string_view entityName, const std::filesystem::path& directory)
	{
		EntityLoadResult result = {};
		Asset::AssimpModelLoader modelLoader;
		const EntityId entityId = scene.CreateEntity(entityName);
		static_cast<void>(scene.EnsureTransformComponent(entityId));
		static_cast<void>(scene.EnsureBoundsComponent(entityId));
		auto& meshComponent = scene.EnsureMeshComponent(entityId);

		std::vector<std::filesystem::path> modelCandidates;
		for (const auto& entry : std::filesystem::directory_iterator(directory))
		{
			if (!entry.is_regular_file())
			{
				continue;
			}

			const auto extension = entry.path().extension().wstring();
			if (extension != L".fbx" && extension != L".obj" && extension != L".gltf" && extension != L".glb")
			{
				continue;
			}

			modelCandidates.push_back(entry.path());
		}

		std::sort(modelCandidates.begin(), modelCandidates.end());

		auto tryLoadModel = [&](bool requireAnimated) -> bool
		{
			for (const auto& path : modelCandidates)
			{
				const bool isAnimated = modelLoader.HasAnimation(path.string());
				if (isAnimated != requireAnimated)
				{
					continue;
				}

				auto loadedMesh = isAnimated
					? modelLoader.LoadAnimatedMesh(path.string())
					: modelLoader.LoadStaticMesh(path.string());
				if (!loadedMesh || loadedMesh->Vertices.empty() || loadedMesh->Indices.empty())
				{
					continue;
				}

				result.Entity = entityId;
				result.SelectedPath = path;
				result.IsAnimated = isAnimated;
				meshComponent.Asset = std::move(loadedMesh);
				meshComponent.MaterialTextures.clear();
				return true;
			}

			return false;
		};

		if (tryLoadModel(true))
		{
			return result;
		}

		static_cast<void>(tryLoadModel(false));

		return result;
	}
}
