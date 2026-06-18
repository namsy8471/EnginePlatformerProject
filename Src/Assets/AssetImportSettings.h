#pragma once

#include <DirectXMath.h>

#include <filesystem>
#include <string>

namespace Asset
{
	struct ModelImportSettings
	{
		float Scale = 1.0f;
		bool ImportMaterials = true;
		bool ImportAnimations = true;
		bool GenerateColliders = false;
		bool GenerateTangents = true;
		bool NormalYFlip = false;
		DirectX::XMFLOAT3 RotationOffset = { 0.0f, 0.0f, 0.0f };
	};

	struct TextureImportSettings
	{
		bool Srgb = true;
		bool GenerateMips = true;
		bool NormalMap = false;
		bool ClampToEdge = false;
	};

	struct AssetImportSettings
	{
		uint32_t FileVersion = 1;
		std::filesystem::path SourcePath;
		ModelImportSettings Model;
		TextureImportSettings Texture;
	};

	struct AssetImportSettingsResult
	{
		bool Success = false;
		AssetImportSettings Settings;
		std::string ErrorMessage;
	};

	class AssetImportSettingsService
	{
	public:
		[[nodiscard]] static std::filesystem::path GetSettingsPathForAsset(const std::filesystem::path& assetPath);
		[[nodiscard]] static AssetImportSettingsResult LoadOrDefault(const std::filesystem::path& assetPath);
		[[nodiscard]] static bool Save(const AssetImportSettings& settings, std::string& errorMessage);
	};
}
