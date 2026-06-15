#include "ProjectService.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <utility>

namespace Projects
{
	namespace
	{
		[[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path, std::string& errorMessage)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
			{
				errorMessage = "Project file could not be opened: " + path.string();
				return {};
			}

			return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		}

		[[nodiscard]] bool WriteTextFile(const std::filesystem::path& path, std::string_view text, std::string& errorMessage)
		{
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				errorMessage = "File could not be written: " + path.string();
				return false;
			}

			stream.write(text.data(), static_cast<std::streamsize>(text.size()));
			return stream.good();
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

		[[nodiscard]] bool CreateDirectoryIfMissing(const std::filesystem::path& path, std::string& errorMessage)
		{
			std::error_code errorCode;
			std::filesystem::create_directories(path, errorCode);
			if (errorCode)
			{
				errorMessage = "Directory could not be created: " + path.string() + " | " + errorCode.message();
				return false;
			}
			return true;
		}
	}

	ProjectResult ProjectService::CreateProject(const ProjectCreateRequest& request)
	{
		ProjectResult result;
		if (request.Name.empty())
		{
			result.ErrorMessage = "Project name is empty.";
			return result;
		}
		if (request.ParentDirectory.empty())
		{
			result.ErrorMessage = "Project parent directory is empty.";
			return result;
		}

		ProjectDescriptor descriptor;
		descriptor.Name = request.Name;
		descriptor.EngineVersion = request.EngineVersion.empty() ? std::string(kCurrentEngineVersion) : request.EngineVersion;
		descriptor.ProjectFilePath = GetProjectFilePath(request).lexically_normal();
		descriptor.RootPath = descriptor.ProjectFilePath.parent_path().lexically_normal();

		std::string errorMessage;
		if (!CreateDirectoryIfMissing(descriptor.RootPath, errorMessage) ||
			!CreateDirectoryIfMissing(descriptor.RootPath / descriptor.AssetRoot, errorMessage) ||
			!CreateDirectoryIfMissing(descriptor.RootPath / descriptor.ScenesRoot, errorMessage) ||
			!CreateDirectoryIfMissing(descriptor.RootPath / descriptor.SettingsRoot, errorMessage) ||
			!CreateDirectoryIfMissing(descriptor.RootPath / "Library", errorMessage) ||
			!CreateDirectoryIfMissing(descriptor.RootPath / "Temp", errorMessage))
		{
			result.ErrorMessage = errorMessage;
			return result;
		}

		const std::filesystem::path startupScenePath = descriptor.RootPath / descriptor.StartupScene;
		if (!std::filesystem::exists(startupScenePath))
		{
			constexpr std::string_view scenePlaceholder =
				"{\n"
				"  \"fileVersion\": 1,\n"
				"  \"name\": \"Main\",\n"
				"  \"entities\": []\n"
				"}\n";
			if (!WriteTextFile(startupScenePath, scenePlaceholder, errorMessage))
			{
				result.ErrorMessage = errorMessage;
				return result;
			}
		}

		const std::filesystem::path gitignorePath = descriptor.RootPath / ".gitignore";
		if (!std::filesystem::exists(gitignorePath))
		{
			constexpr std::string_view gitignore =
				"Library/\n"
				"Temp/\n"
				"Build/\n"
				"Intermediate/\n"
				"*.user\n"
				"*.suo\n";
			if (!WriteTextFile(gitignorePath, gitignore, errorMessage))
			{
				result.ErrorMessage = errorMessage;
				return result;
			}
		}

		if (!WriteDescriptor(descriptor, errorMessage))
		{
			result.ErrorMessage = errorMessage;
			return result;
		}

		return ValidateProject(std::move(descriptor));
	}

	ProjectResult ProjectService::LoadProject(const std::filesystem::path& projectFilePath)
	{
		ProjectResult result;
		std::string errorMessage;
		const std::string text = ReadTextFile(projectFilePath, errorMessage);
		if (!errorMessage.empty())
		{
			result.ErrorMessage = errorMessage;
			return result;
		}

		rapidjson::Document document;
		document.Parse(text.c_str(), text.size());
		if (document.HasParseError() || !document.IsObject())
		{
			result.ErrorMessage = "Project JSON parse failed: ";
			result.ErrorMessage.append(rapidjson::GetParseError_En(document.GetParseError()));
			return result;
		}

		ProjectDescriptor descriptor;
		descriptor.FileVersion = JsonUintOrDefault(document, "fileVersion", kProjectFileVersion);
		descriptor.EngineVersion = JsonStringOrDefault(document, "engineVersion", kCurrentEngineVersion);
		descriptor.Name = JsonStringOrDefault(document, "name", projectFilePath.stem().string());
		descriptor.AssetRoot = JsonStringOrDefault(document, "assetRoot", "Assets");
		descriptor.ScenesRoot = JsonStringOrDefault(document, "scenesRoot", "Scenes");
		descriptor.SettingsRoot = JsonStringOrDefault(document, "settingsRoot", "Settings");
		descriptor.StartupScene = JsonStringOrDefault(document, "startupScene", "Scenes/Main.scene");
		descriptor.ProjectFilePath = std::filesystem::absolute(projectFilePath).lexically_normal();
		descriptor.RootPath = descriptor.ProjectFilePath.parent_path().lexically_normal();

		return ValidateProject(std::move(descriptor));
	}

	ProjectResult ProjectService::ValidateProject(ProjectDescriptor descriptor)
	{
		ProjectResult result;
		if (descriptor.Name.empty())
		{
			result.ErrorMessage = "Project name is empty.";
			return result;
		}
		if (descriptor.ProjectFilePath.empty() || !std::filesystem::is_regular_file(descriptor.ProjectFilePath))
		{
			result.ErrorMessage = "Project file does not exist: " + descriptor.ProjectFilePath.string();
			return result;
		}
		if (!std::filesystem::is_directory(descriptor.RootPath / descriptor.AssetRoot))
		{
			result.ErrorMessage = "Project Assets folder does not exist: " + (descriptor.RootPath / descriptor.AssetRoot).string();
			return result;
		}

		result.Success = true;
		result.Descriptor = std::move(descriptor);
		return result;
	}

	std::filesystem::path ProjectService::GetProjectFilePath(const ProjectCreateRequest& request)
	{
		const std::string directoryName = SanitizeProjectDirectoryName(request.Name);
		return request.ParentDirectory / directoryName / (directoryName + ".engineproject");
	}

	bool ProjectService::IsProjectFile(const std::filesystem::path& path)
	{
		return path.extension() == ".engineproject";
	}

	bool ProjectService::WriteDescriptor(const ProjectDescriptor& descriptor, std::string& errorMessage)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
		writer.StartObject();
		writer.Key("fileVersion");
		writer.Uint(descriptor.FileVersion);
		writer.Key("engineVersion");
		writer.String(descriptor.EngineVersion.c_str());
		writer.Key("name");
		writer.String(descriptor.Name.c_str());
		writer.Key("assetRoot");
		writer.String(descriptor.AssetRoot.generic_string().c_str());
		writer.Key("scenesRoot");
		writer.String(descriptor.ScenesRoot.generic_string().c_str());
		writer.Key("settingsRoot");
		writer.String(descriptor.SettingsRoot.generic_string().c_str());
		writer.Key("startupScene");
		writer.String(descriptor.StartupScene.generic_string().c_str());
		writer.EndObject();

		return WriteTextFile(descriptor.ProjectFilePath, std::string_view(buffer.GetString(), buffer.GetSize()), errorMessage);
	}

	std::string ProjectService::SanitizeProjectDirectoryName(std::string name)
	{
		constexpr std::string_view invalidCharacters = "<>:\"/\\|?*";
		std::replace_if(name.begin(), name.end(), [](char character)
			{
				const unsigned char value = static_cast<unsigned char>(character);
				return std::iscntrl(value) || invalidCharacters.find(character) != std::string_view::npos;
			}, '_');

		while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
		{
			name.pop_back();
		}
		return name.empty() ? "NewProject" : name;
	}
}
