#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

#include "Rendering/RHI/GraphicsCommon.h"
#include "Rendering/RenderMode.h"

struct EngineStartupOptions
{
	std::optional<std::filesystem::path> ProjectFilePath;
	std::optional<std::filesystem::path> RuntimePackageManifestPath;
	std::optional<std::uint32_t> SmokeTestFrameLimit;
	std::optional<std::filesystem::path> SmokeLogPath;
	std::optional<GraphicsAPI> SmokeGraphicsApi;
	std::optional<RenderMode> SmokeRenderMode;
};
