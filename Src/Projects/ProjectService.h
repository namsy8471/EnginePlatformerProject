#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Projects
{
	inline constexpr uint32_t kProjectFileVersion = 1;
	inline constexpr std::string_view kCurrentEngineVersion = "0.1.0";

	struct ProjectDescriptor
	{
		uint32_t FileVersion = kProjectFileVersion;
		std::string EngineVersion = std::string(kCurrentEngineVersion);
		std::string Name;
		std::filesystem::path ProjectFilePath;
		std::filesystem::path RootPath;
		std::filesystem::path AssetRoot = "Assets";
		std::filesystem::path ScenesRoot = "Scenes";
		std::filesystem::path SettingsRoot = "Settings";
		std::filesystem::path StartupScene = "Scenes/Main.scene";
	};

	struct ProjectCreateRequest
	{
		std::string Name;
		std::filesystem::path ParentDirectory;
		std::string EngineVersion = std::string(kCurrentEngineVersion);
	};

	struct ProjectResult
	{
		bool Success = false;
		ProjectDescriptor Descriptor;
		std::string ErrorMessage;
	};

	class ProjectService
	{
	public:
		[[nodiscard]] static ProjectResult CreateProject(const ProjectCreateRequest& request);
		[[nodiscard]] static ProjectResult LoadProject(const std::filesystem::path& projectFilePath);
		[[nodiscard]] static ProjectResult ValidateProject(ProjectDescriptor descriptor);
		[[nodiscard]] static std::filesystem::path GetProjectFilePath(const ProjectCreateRequest& request);
		[[nodiscard]] static bool IsProjectFile(const std::filesystem::path& path);

	private:
		[[nodiscard]] static bool WriteDescriptor(const ProjectDescriptor& descriptor, std::string& errorMessage);
		[[nodiscard]] static std::string SanitizeProjectDirectoryName(std::string name);
	};
}
