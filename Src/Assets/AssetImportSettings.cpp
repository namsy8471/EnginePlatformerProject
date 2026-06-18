#include "AssetImportSettings.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <fstream>
#include <iterator>
#include <system_error>

namespace Asset
{
	namespace
	{
		[[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path, std::string& errorMessage)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
			{
				errorMessage = "Import settings file could not be opened: " + path.string();
				return {};
			}
			return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		}

		[[nodiscard]] bool WriteTextFile(const std::filesystem::path& path, std::string_view text, std::string& errorMessage)
		{
			std::error_code errorCode;
			std::filesystem::create_directories(path.parent_path(), errorCode);
			if (errorCode)
			{
				errorMessage = "Import settings directory could not be created: " + errorCode.message();
				return false;
			}

			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				errorMessage = "Import settings file could not be written: " + path.string();
				return false;
			}
			stream.write(text.data(), static_cast<std::streamsize>(text.size()));
			return stream.good();
		}

		[[nodiscard]] bool BoolMemberOrDefault(const rapidjson::Value& object, const char* key, bool fallback)
		{
			return object.HasMember(key) && object[key].IsBool() ? object[key].GetBool() : fallback;
		}

		[[nodiscard]] float FloatMemberOrDefault(const rapidjson::Value& object, const char* key, float fallback)
		{
			return object.HasMember(key) && object[key].IsNumber() ? object[key].GetFloat() : fallback;
		}

		[[nodiscard]] uint32_t UintMemberOrDefault(const rapidjson::Value& object, const char* key, uint32_t fallback)
		{
			return object.HasMember(key) && object[key].IsUint() ? object[key].GetUint() : fallback;
		}

		void ReadFloat3(const rapidjson::Value& object, const char* key, DirectX::XMFLOAT3& value)
		{
			if (!object.HasMember(key) || !object[key].IsArray() || object[key].Size() != 3)
			{
				return;
			}

			const auto& array = object[key];
			if (array[0].IsNumber() && array[1].IsNumber() && array[2].IsNumber())
			{
				value = { array[0].GetFloat(), array[1].GetFloat(), array[2].GetFloat() };
			}
		}

		template <typename Writer>
		void WriteFloat3(Writer& writer, const char* key, const DirectX::XMFLOAT3& value)
		{
			writer.Key(key);
			writer.StartArray();
			writer.Double(value.x);
			writer.Double(value.y);
			writer.Double(value.z);
			writer.EndArray();
		}
	}

	std::filesystem::path AssetImportSettingsService::GetSettingsPathForAsset(const std::filesystem::path& assetPath)
	{
		std::filesystem::path settingsPath = assetPath;
		settingsPath += ".import.json";
		return settingsPath;
	}

	AssetImportSettingsResult AssetImportSettingsService::LoadOrDefault(const std::filesystem::path& assetPath)
	{
		AssetImportSettingsResult result;
		result.Settings.SourcePath = assetPath;

		const std::filesystem::path settingsPath = GetSettingsPathForAsset(assetPath);
		if (!std::filesystem::exists(settingsPath))
		{
			result.Success = true;
			return result;
		}

		std::string errorMessage;
		const std::string text = ReadTextFile(settingsPath, errorMessage);
		if (!errorMessage.empty())
		{
			result.ErrorMessage = errorMessage;
			return result;
		}

		rapidjson::Document document;
		document.Parse(text.c_str(), text.size());
		if (document.HasParseError() || !document.IsObject())
		{
			result.ErrorMessage = "Import settings JSON parse failed: ";
			result.ErrorMessage.append(rapidjson::GetParseError_En(document.GetParseError()));
			return result;
		}

		result.Settings.FileVersion = UintMemberOrDefault(document, "fileVersion", result.Settings.FileVersion);
		if (document.HasMember("model") && document["model"].IsObject())
		{
			const auto& model = document["model"];
			result.Settings.Model.Scale = FloatMemberOrDefault(model, "scale", result.Settings.Model.Scale);
			result.Settings.Model.ImportMaterials = BoolMemberOrDefault(model, "importMaterials", result.Settings.Model.ImportMaterials);
			result.Settings.Model.ImportAnimations = BoolMemberOrDefault(model, "importAnimations", result.Settings.Model.ImportAnimations);
			result.Settings.Model.GenerateColliders = BoolMemberOrDefault(model, "generateColliders", result.Settings.Model.GenerateColliders);
			result.Settings.Model.GenerateTangents = BoolMemberOrDefault(model, "generateTangents", result.Settings.Model.GenerateTangents);
			result.Settings.Model.NormalYFlip = BoolMemberOrDefault(model, "normalYFlip", result.Settings.Model.NormalYFlip);
			ReadFloat3(model, "rotationOffset", result.Settings.Model.RotationOffset);
		}
		if (document.HasMember("texture") && document["texture"].IsObject())
		{
			const auto& texture = document["texture"];
			result.Settings.Texture.Srgb = BoolMemberOrDefault(texture, "srgb", result.Settings.Texture.Srgb);
			result.Settings.Texture.GenerateMips = BoolMemberOrDefault(texture, "generateMips", result.Settings.Texture.GenerateMips);
			result.Settings.Texture.NormalMap = BoolMemberOrDefault(texture, "normalMap", result.Settings.Texture.NormalMap);
			result.Settings.Texture.ClampToEdge = BoolMemberOrDefault(texture, "clampToEdge", result.Settings.Texture.ClampToEdge);
		}

		result.Success = true;
		return result;
	}

	bool AssetImportSettingsService::Save(const AssetImportSettings& settings, std::string& errorMessage)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);

		writer.StartObject();
		writer.Key("fileVersion");
		writer.Uint(settings.FileVersion);
		writer.Key("model");
		writer.StartObject();
		writer.Key("scale");
		writer.Double(settings.Model.Scale);
		writer.Key("importMaterials");
		writer.Bool(settings.Model.ImportMaterials);
		writer.Key("importAnimations");
		writer.Bool(settings.Model.ImportAnimations);
		writer.Key("generateColliders");
		writer.Bool(settings.Model.GenerateColliders);
		writer.Key("generateTangents");
		writer.Bool(settings.Model.GenerateTangents);
		writer.Key("normalYFlip");
		writer.Bool(settings.Model.NormalYFlip);
		WriteFloat3(writer, "rotationOffset", settings.Model.RotationOffset);
		writer.EndObject();
		writer.Key("texture");
		writer.StartObject();
		writer.Key("srgb");
		writer.Bool(settings.Texture.Srgb);
		writer.Key("generateMips");
		writer.Bool(settings.Texture.GenerateMips);
		writer.Key("normalMap");
		writer.Bool(settings.Texture.NormalMap);
		writer.Key("clampToEdge");
		writer.Bool(settings.Texture.ClampToEdge);
		writer.EndObject();
		writer.EndObject();

		return WriteTextFile(GetSettingsPathForAsset(settings.SourcePath), std::string_view(buffer.GetString(), buffer.GetSize()), errorMessage);
	}
}
