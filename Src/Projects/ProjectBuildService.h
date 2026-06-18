#pragma once

#include "Projects/ProjectService.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Projects
{
	struct ProjectBuildRequest
	{
		ProjectDescriptor Project;
		std::filesystem::path OutputDirectory;
		bool CopyAssets = true;
		bool CopyScenes = true;
		bool WriteManifest = true;
	};

	struct ProjectBuildResult
	{
		bool Success = false;
		std::filesystem::path OutputDirectory;
		std::vector<std::filesystem::path> WrittenFiles;
		std::string ErrorMessage;
	};

	struct RuntimePackageResult
	{
		bool Success = false;
		ProjectDescriptor Descriptor;
		std::filesystem::path ManifestPath;
		std::string ErrorMessage;
	};

	class ProjectBuildService
	{
	public:
		[[nodiscard]] static ProjectBuildResult BuildRuntimePackage(const ProjectBuildRequest& request);
		[[nodiscard]] static RuntimePackageResult LoadRuntimePackage(const std::filesystem::path& manifestPath);
	};
}
