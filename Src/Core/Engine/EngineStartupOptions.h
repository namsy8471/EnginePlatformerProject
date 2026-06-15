#pragma once

#include <filesystem>
#include <optional>

struct EngineStartupOptions
{
	std::optional<std::filesystem::path> ProjectFilePath;
};
