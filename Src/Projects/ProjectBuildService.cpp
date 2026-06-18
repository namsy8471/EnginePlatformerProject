#include "ProjectBuildService.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace Projects
{
	namespace
	{
		[[nodiscard]] bool CopyDirectoryTree(
			const std::filesystem::path& source,
			const std::filesystem::path& destination,
			std::vector<std::filesystem::path>& writtenFiles,
			std::string& errorMessage)
		{
			std::error_code errorCode;
			if (!std::filesystem::exists(source, errorCode))
			{
				return true;
			}

			for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(source, errorCode))
			{
				if (errorCode)
				{
					errorMessage = "Export directory traversal failed: " + errorCode.message();
					return false;
				}

				const std::filesystem::path relativePath = std::filesystem::relative(entry.path(), source, errorCode);
				if (errorCode)
				{
					errorMessage = "Export relative path failed: " + errorCode.message();
					return false;
				}

				const std::filesystem::path targetPath = destination / relativePath;
				if (entry.is_directory())
				{
					std::filesystem::create_directories(targetPath, errorCode);
					if (errorCode)
					{
						errorMessage = "Export directory create failed: " + errorCode.message();
						return false;
					}
					continue;
				}

				if (!entry.is_regular_file())
				{
					continue;
				}

				std::filesystem::create_directories(targetPath.parent_path(), errorCode);
				if (errorCode)
				{
					errorMessage = "Export file directory create failed: " + errorCode.message();
					return false;
				}

				std::filesystem::copy_file(entry.path(), targetPath, std::filesystem::copy_options::overwrite_existing, errorCode);
				if (errorCode)
				{
					errorMessage = "Export copy failed: " + entry.path().string() + " | " + errorCode.message();
					return false;
				}
				writtenFiles.push_back(targetPath);
			}

			return true;
		}

		[[nodiscard]] bool WriteManifest(
			const ProjectBuildRequest& request,
			const std::filesystem::path& manifestPath,
			std::vector<std::filesystem::path>& writtenFiles,
			std::string& errorMessage)
		{
			rapidjson::StringBuffer buffer;
			rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
			writer.StartObject();
			writer.Key("fileVersion");
			writer.Uint(1);
			writer.Key("projectName");
			writer.String(request.Project.Name.c_str());
			writer.Key("engineVersion");
			writer.String(request.Project.EngineVersion.c_str());
			writer.Key("startupScene");
			writer.String(request.Project.StartupScene.generic_string().c_str());
			writer.Key("assetRoot");
			writer.String(request.Project.AssetRoot.generic_string().c_str());
			writer.Key("scenesRoot");
			writer.String(request.Project.ScenesRoot.generic_string().c_str());
			writer.Key("settingsRoot");
			writer.String(request.Project.SettingsRoot.generic_string().c_str());
			writer.EndObject();

			std::error_code errorCode;
			std::filesystem::create_directories(manifestPath.parent_path(), errorCode);
			if (errorCode)
			{
				errorMessage = "Export manifest directory create failed: " + errorCode.message();
				return false;
			}

			std::ofstream stream(manifestPath, std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				errorMessage = "Export manifest could not be written: " + manifestPath.string();
				return false;
			}
			stream.write(buffer.GetString(), static_cast<std::streamsize>(buffer.GetSize()));
			if (!stream.good())
			{
				errorMessage = "Export manifest write failed: " + manifestPath.string();
				return false;
			}
			writtenFiles.push_back(manifestPath);
			return true;
		}

		[[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path, std::string& errorMessage)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
			{
				errorMessage = "Runtime package manifest could not be opened: " + path.string();
				return {};
			}

			return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		}

		[[nodiscard]] std::string JsonStringOrDefault(const rapidjson::Value& object, const char* name, std::string_view fallback)
		{
			if (!object.HasMember(name) || !object[name].IsString())
			{
				return std::string(fallback);
			}
			return object[name].GetString();
		}

		[[nodiscard]] uint32_t JsonUintOrDefault(const rapidjson::Value& object, const char* name, uint32_t fallback)
		{
			if (!object.HasMember(name) || !object[name].IsUint())
			{
				return fallback;
			}
			return object[name].GetUint();
		}
	}

	ProjectBuildResult ProjectBuildService::BuildRuntimePackage(const ProjectBuildRequest& request)
	{
		ProjectBuildResult result;
		result.OutputDirectory = request.OutputDirectory;
		if (request.Project.RootPath.empty() || request.OutputDirectory.empty())
		{
			result.ErrorMessage = "Export requires a valid project root and output directory.";
			return result;
		}

		std::error_code errorCode;
		std::filesystem::create_directories(request.OutputDirectory, errorCode);
		if (errorCode)
		{
			result.ErrorMessage = "Export output directory could not be created: " + errorCode.message();
			return result;
		}

		if (request.CopyAssets)
		{
			if (!CopyDirectoryTree(
				request.Project.RootPath / request.Project.AssetRoot,
				request.OutputDirectory / request.Project.AssetRoot,
				result.WrittenFiles,
				result.ErrorMessage))
			{
				return result;
			}
		}

		if (request.CopyScenes)
		{
			if (!CopyDirectoryTree(
				request.Project.RootPath / request.Project.ScenesRoot,
				request.OutputDirectory / request.Project.ScenesRoot,
				result.WrittenFiles,
				result.ErrorMessage))
			{
				return result;
			}
		}

		if (request.WriteManifest)
		{
			if (!WriteManifest(request, request.OutputDirectory / "runtime-package.json", result.WrittenFiles, result.ErrorMessage))
			{
				return result;
			}
		}

		result.Success = true;
		return result;
	}

	RuntimePackageResult ProjectBuildService::LoadRuntimePackage(const std::filesystem::path& manifestPath)
	{
		RuntimePackageResult result;
		result.ManifestPath = std::filesystem::absolute(manifestPath).lexically_normal();
		std::string errorMessage;
		const std::string text = ReadTextFile(result.ManifestPath, errorMessage);
		if (!errorMessage.empty())
		{
			result.ErrorMessage = errorMessage;
			return result;
		}

		rapidjson::Document document;
		document.Parse(text.c_str(), text.size());
		if (document.HasParseError() || !document.IsObject())
		{
			result.ErrorMessage = "Runtime package manifest JSON parse failed: ";
			result.ErrorMessage.append(rapidjson::GetParseError_En(document.GetParseError()));
			return result;
		}

		ProjectDescriptor descriptor;
		descriptor.FileVersion = JsonUintOrDefault(document, "fileVersion", kProjectFileVersion);
		descriptor.EngineVersion = JsonStringOrDefault(document, "engineVersion", kCurrentEngineVersion);
		descriptor.Name = JsonStringOrDefault(document, "projectName", result.ManifestPath.parent_path().filename().string());
		descriptor.AssetRoot = JsonStringOrDefault(document, "assetRoot", "Assets");
		descriptor.ScenesRoot = JsonStringOrDefault(document, "scenesRoot", "Scenes");
		descriptor.SettingsRoot = JsonStringOrDefault(document, "settingsRoot", "Settings");
		descriptor.StartupScene = JsonStringOrDefault(document, "startupScene", "Scenes/Main.scene");
		descriptor.ProjectFilePath = result.ManifestPath;
		descriptor.RootPath = result.ManifestPath.parent_path().lexically_normal();

		std::error_code errorCode;
		if (!std::filesystem::is_directory(descriptor.RootPath / descriptor.AssetRoot, errorCode))
		{
			result.ErrorMessage = "Runtime package Assets folder does not exist: " + (descriptor.RootPath / descriptor.AssetRoot).string();
			return result;
		}
		if (!std::filesystem::is_regular_file(descriptor.RootPath / descriptor.StartupScene, errorCode))
		{
			result.ErrorMessage = "Runtime package startup scene does not exist: " + (descriptor.RootPath / descriptor.StartupScene).string();
			return result;
		}

		result.Success = true;
		result.Descriptor = std::move(descriptor);
		return result;
	}
}
