#include "Rendering/Sky/SkyboxAsset.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace Rendering
{
	namespace
	{
		[[nodiscard]] std::string ToLower(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
				{
					return static_cast<char>(std::tolower(character));
				});
			return value;
		}

		[[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path, std::string& errorMessage)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
			{
				errorMessage = "Skybox asset could not be opened: " + path.string();
				return {};
			}

			return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		}

		[[nodiscard]] float JsonFloatOrDefault(const rapidjson::Value& object, const char* name, float fallback) noexcept
		{
			if (!object.IsObject() || !object.HasMember(name))
			{
				return fallback;
			}

			const rapidjson::Value& value = object[name];
			if (!value.IsNumber())
			{
				return fallback;
			}

			return value.GetFloat();
		}

		[[nodiscard]] bool JsonBoolOrDefault(const rapidjson::Value& object, const char* name, bool fallback) noexcept
		{
			if (!object.IsObject() || !object.HasMember(name) || !object[name].IsBool())
			{
				return fallback;
			}
			return object[name].GetBool();
		}

		[[nodiscard]] DirectX::XMFLOAT3 JsonFloat3OrDefault(const rapidjson::Value& object, const char* name, const DirectX::XMFLOAT3& fallback) noexcept
		{
			if (!object.IsObject() || !object.HasMember(name))
			{
				return fallback;
			}

			const rapidjson::Value& value = object[name];
			if (!value.IsArray() || value.Size() < 3)
			{
				return fallback;
			}

			return {
				value[0].IsNumber() ? value[0].GetFloat() : fallback.x,
				value[1].IsNumber() ? value[1].GetFloat() : fallback.y,
				value[2].IsNumber() ? value[2].GetFloat() : fallback.z
			};
		}

		template <typename Writer>
		void WriteFloat3(Writer& writer, const char* name, const DirectX::XMFLOAT3& value)
		{
			writer.Key(name);
			writer.StartArray();
			writer.Double(value.x);
			writer.Double(value.y);
			writer.Double(value.z);
			writer.EndArray();
		}

		template <typename Writer>
		void WriteSkyboxObject(Writer& writer, const SkyboxSettings& sourceSettings)
		{
			const SkyboxSettings settings = ClampSkyboxSettings(sourceSettings);

			writer.StartObject();
			writer.Key("enabled");
			writer.Bool(settings.Enabled);
			WriteFloat3(writer, "zenithColor", settings.ZenithColor);
			WriteFloat3(writer, "horizonColor", settings.HorizonColor);
			WriteFloat3(writer, "groundColor", settings.GroundColor);
			WriteFloat3(writer, "sunColor", settings.SunColor);
			WriteFloat3(writer, "sunDirection", settings.SunDirection);
			writer.Key("intensity");
			writer.Double(settings.Intensity);
			writer.Key("horizonHeight");
			writer.Double(settings.HorizonHeight);
			writer.Key("horizonBlend");
			writer.Double(settings.HorizonBlend);
			writer.Key("sunSize");
			writer.Double(settings.SunSize);
			writer.Key("sunIntensity");
			writer.Double(settings.SunIntensity);
			writer.EndObject();
		}
	}

	bool IsSkyboxAssetPath(const std::filesystem::path& path)
	{
		return ToLower(path.extension().string()) == ".skybox";
	}

	std::string BuildSkyboxAssetJson(const SkyboxSettings& settings)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
		writer.StartObject();
		writer.Key("fileVersion");
		writer.Uint(1);
		writer.Key("type");
		writer.String("ProceduralSkybox");
		writer.Key("skybox");
		WriteSkyboxObject(writer, settings);
		writer.EndObject();
		return std::string(buffer.GetString(), buffer.GetSize());
	}

	SkyboxAssetLoadResult LoadSkyboxAsset(const std::filesystem::path& path)
	{
		SkyboxAssetLoadResult result;
		std::string errorMessage;
		const std::string text = ReadTextFile(path, errorMessage);
		if (!errorMessage.empty())
		{
			result.ErrorMessage = std::move(errorMessage);
			return result;
		}

		rapidjson::Document document;
		document.Parse(text.c_str(), text.size());
		if (document.HasParseError() || !document.IsObject())
		{
			result.ErrorMessage = "Skybox asset JSON parse failed: " + path.string();
			return result;
		}

		const rapidjson::Value* skyboxObject = &document;
		if (document.HasMember("skybox") && document["skybox"].IsObject())
		{
			skyboxObject = &document["skybox"];
		}
		else if (document.HasMember("settings") && document["settings"].IsObject())
		{
			skyboxObject = &document["settings"];
		}

		SkyboxSettings settings;
		settings.Enabled = JsonBoolOrDefault(*skyboxObject, "enabled", settings.Enabled);
		settings.ZenithColor = JsonFloat3OrDefault(*skyboxObject, "zenithColor", settings.ZenithColor);
		settings.HorizonColor = JsonFloat3OrDefault(*skyboxObject, "horizonColor", settings.HorizonColor);
		settings.GroundColor = JsonFloat3OrDefault(*skyboxObject, "groundColor", settings.GroundColor);
		settings.SunColor = JsonFloat3OrDefault(*skyboxObject, "sunColor", settings.SunColor);
		settings.SunDirection = JsonFloat3OrDefault(*skyboxObject, "sunDirection", settings.SunDirection);
		settings.Intensity = JsonFloatOrDefault(*skyboxObject, "intensity", settings.Intensity);
		settings.HorizonHeight = JsonFloatOrDefault(*skyboxObject, "horizonHeight", settings.HorizonHeight);
		settings.HorizonBlend = JsonFloatOrDefault(*skyboxObject, "horizonBlend", settings.HorizonBlend);
		settings.SunSize = JsonFloatOrDefault(*skyboxObject, "sunSize", settings.SunSize);
		settings.SunIntensity = JsonFloatOrDefault(*skyboxObject, "sunIntensity", settings.SunIntensity);

		result.Success = true;
		result.Settings = ClampSkyboxSettings(settings);
		return result;
	}

	bool SaveSkyboxAsset(const std::filesystem::path& path, const SkyboxSettings& settings, std::string& errorMessage)
	{
		const std::filesystem::path parentPath = path.parent_path();
		if (!parentPath.empty())
		{
			std::error_code errorCode;
			std::filesystem::create_directories(parentPath, errorCode);
			if (errorCode)
			{
				errorMessage = "Skybox asset directory could not be created: " + errorCode.message();
				return false;
			}
		}

		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		if (!stream)
		{
			errorMessage = "Skybox asset could not be written: " + path.string();
			return false;
		}

		const std::string json = BuildSkyboxAssetJson(settings);
		stream.write(json.data(), static_cast<std::streamsize>(json.size()));
		stream.put('\n');
		return true;
	}
}
