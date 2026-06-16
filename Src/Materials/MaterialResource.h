#pragma once

#include "Assets/StaticMesh.h"
#include "Resources/ResourceTypes.h"

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Materials
{
	struct MaterialTextureSlotResource
	{
		Asset::MaterialTextureSlot Slot = Asset::MaterialTextureSlot::BaseColor;
		std::filesystem::path SourcePath;
		bool IsOverride = false;
		bool IsEmbedded = false;
		bool Srgb = true;
	};

	struct MaterialResource
	{
		Resources::ResourceHandle Handle;
		std::string Name;
		Asset::MaterialShadingModel ShadingModel = Asset::MaterialShadingModel::Phong;
		DirectX::XMFLOAT4 BaseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT4 ImportedDiffuseTint = { 1.0f, 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT3 SpecularColor = { 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT3 EmissiveColor = { 0.0f, 0.0f, 0.0f };
		float MetallicFactor = 0.0f;
		float RoughnessFactor = 0.5f;
		float Shininess = 32.0f;
		float Opacity = 1.0f;
		bool UseVertexColor = false;
		bool NormalYFlip = false;
		std::array<MaterialTextureSlotResource, Asset::kMaterialTextureSlotCount> TextureSlots = {};
	};

	struct MaterialResourceStats
	{
		size_t MaterialCount = 0;
		size_t TextureSlotCount = 0;
		size_t OverrideSlotCount = 0;
		size_t EmbeddedSlotCount = 0;
		size_t PbrCount = 0;
		size_t PhongCount = 0;
		size_t UnlitCount = 0;
	};

	[[nodiscard]] MaterialResource BuildMaterialResource(
		const Asset::StaticMeshMaterial& material,
		Resources::ResourceHandle handle = {});

	[[nodiscard]] std::vector<MaterialResource> BuildMaterialResources(
		const Asset::StaticMeshAsset& meshAsset);

	[[nodiscard]] MaterialResourceStats BuildMaterialResourceStats(
		const std::vector<MaterialResource>& materials);
}
