#pragma once

#include "Projects/ProjectService.h"
#include "Scene/Scene.h"
#include "Scene/ScenePersistenceService.h"

#include <filesystem>
#include <string>

namespace ScenePersistence
{
	struct PrefabSaveOptions
	{
		DirectX::XMFLOAT3 AmbientColor = { 0.62f, 0.68f, 0.78f };
		float AmbientIntensity = 0.35f;
		float Exposure = 1.0f;
		Rendering::SkyboxSettings Skybox;
		bool IncludePrefabInstanceComponent = true;
	};

	struct LoadPrefabResult
	{
		bool Success = false;
		std::string ErrorMessage;
		LoadedSceneEntity Root;
	};

	class PrefabService
	{
	public:
		[[nodiscard]] static bool SaveEntityAsPrefab(
			const Scene& scene,
			EntityId entityId,
			const Projects::ProjectDescriptor& project,
			const std::filesystem::path& prefabPath,
			const PrefabSaveOptions& options,
			std::string& errorMessage);

		[[nodiscard]] static bool SaveLoadedEntityAsPrefab(
			const LoadedSceneEntity& entity,
			const Projects::ProjectDescriptor& project,
			const std::filesystem::path& prefabPath,
			const PrefabSaveOptions& options,
			std::string& errorMessage);

		[[nodiscard]] static LoadPrefabResult LoadPrefab(
			const std::filesystem::path& prefabPath,
			const Projects::ProjectDescriptor& project);
	};
}
