#pragma once

#include "Rendering/Sky/SkyboxSettings.h"

#include <filesystem>
#include <string>

namespace Rendering
{
	struct SkyboxAssetLoadResult
	{
		bool Success = false;
		SkyboxSettings Settings = {};
		std::string ErrorMessage;
	};

	[[nodiscard]] bool IsSkyboxAssetPath(const std::filesystem::path& path);
	[[nodiscard]] std::string BuildSkyboxAssetJson(const SkyboxSettings& settings);
	[[nodiscard]] SkyboxAssetLoadResult LoadSkyboxAsset(const std::filesystem::path& path);
	[[nodiscard]] bool SaveSkyboxAsset(const std::filesystem::path& path, const SkyboxSettings& settings, std::string& errorMessage);
}
