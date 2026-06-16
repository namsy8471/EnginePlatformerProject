#include "MaterialTextureSystem.h"

#include "Scene/SceneRenderState.h"

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>

namespace Rendering
{
	namespace
	{
		[[nodiscard]] uint8_t ToColorByte(float value) noexcept
		{
			return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
		}

		void SetSolidPixel(CpuMaterialTextureSlot& texture, uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool srgb)
		{
			texture.Path.clear();
			texture.Pixels = { r, g, b, a };
			texture.Width = 1;
			texture.Height = 1;
			texture.Srgb = srgb;
		}

		void SetSlotFallback(CpuMaterialTexture& materialTexture, const Asset::StaticMeshMaterial* material)
		{
			auto& baseColor = materialTexture.Slot(Asset::MaterialTextureSlot::BaseColor);
			SetSolidPixel(
				baseColor,
				ToColorByte(material ? material->DiffuseColor.x : 1.0f),
				ToColorByte(material ? material->DiffuseColor.y : 1.0f),
				ToColorByte(material ? material->DiffuseColor.z : 1.0f),
				ToColorByte(material ? material->DiffuseColor.w * material->Opacity : 1.0f),
				true);

			SetSolidPixel(materialTexture.Slot(Asset::MaterialTextureSlot::Normal), 128, 128, 255, 255, false);
			SetSolidPixel(materialTexture.Slot(Asset::MaterialTextureSlot::Metallic), ToColorByte(material ? material->MetallicFactor : 0.0f), 0, 0, 255, false);
			SetSolidPixel(materialTexture.Slot(Asset::MaterialTextureSlot::Roughness), ToColorByte(material ? material->RoughnessFactor : 0.5f), 0, 0, 255, false);
			SetSolidPixel(
				materialTexture.Slot(Asset::MaterialTextureSlot::MetallicRoughness),
				0,
				ToColorByte(material ? material->RoughnessFactor : 0.5f),
				ToColorByte(material ? material->MetallicFactor : 0.0f),
				255,
				false);
			SetSolidPixel(materialTexture.Slot(Asset::MaterialTextureSlot::AO), 255, 255, 255, 255, false);
			SetSolidPixel(
				materialTexture.Slot(Asset::MaterialTextureSlot::Emissive),
				ToColorByte(material ? material->EmissiveColor.x : 0.0f),
				ToColorByte(material ? material->EmissiveColor.y : 0.0f),
				ToColorByte(material ? material->EmissiveColor.z : 0.0f),
				255,
				true);
			SetSolidPixel(materialTexture.Slot(Asset::MaterialTextureSlot::Opacity), ToColorByte(material ? material->Opacity : 1.0f), 255, 255, 255, false);
			SetSolidPixel(
				materialTexture.Slot(Asset::MaterialTextureSlot::Specular),
				ToColorByte(material ? material->SpecularColor.x : 1.0f),
				ToColorByte(material ? material->SpecularColor.y : 1.0f),
				ToColorByte(material ? material->SpecularColor.z : 1.0f),
				255,
				true);
			SetSolidPixel(materialTexture.Slot(Asset::MaterialTextureSlot::Shininess), ToColorByte((material ? material->Shininess : 32.0f) / 256.0f), 0, 0, 255, false);
		}

		[[nodiscard]] bool LoadTextureFile(
			CpuMaterialTextureSlot& target,
			const std::filesystem::path& texturePath,
			Asset::MaterialTextureSlot slot,
			size_t materialIndex,
			const MaterialTextureSystem::LogCallback& logCallback)
		{
			if (texturePath.empty() || !std::filesystem::exists(texturePath))
			{
				return false;
			}

			int width = 0;
			int height = 0;
			int channels = 0;
			stbi_uc* pixels = stbi_load(texturePath.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
			if (!pixels)
			{
				if (logCallback)
				{
					logCallback(std::format(
						"Material texture load failed - MaterialIndex={} Slot={} Path={} SelectedPath=<fallback>",
						materialIndex,
						Asset::MaterialTextureSlotName(slot),
						texturePath.string()));
				}
				return false;
			}

			target.Path = texturePath;
			target.Width = width;
			target.Height = height;
			target.Srgb = Asset::IsMaterialTextureSlotSrgb(slot);
			target.Pixels.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
			stbi_image_free(pixels);

			if (logCallback)
			{
				logCallback(std::format(
					"Material texture loaded - MaterialIndex={} Slot={} Path={} SourceChannels={} UploadedAs={}",
					materialIndex,
					Asset::MaterialTextureSlotName(slot),
					texturePath.string(),
					channels,
					target.Srgb ? "sRGB RGBA8" : "linear RGBA8"));
			}
			return true;
		}

		void LoadEmbeddedTexture(
			CpuMaterialTextureSlot& target,
			const Asset::EmbeddedMaterialTexture& embedded,
			Asset::MaterialTextureSlot slot,
			size_t materialIndex,
			const MaterialTextureSystem::LogCallback& logCallback)
		{
			if (!embedded.IsValid())
			{
				return;
			}

			target.Path.clear();
			target.Width = embedded.Width;
			target.Height = embedded.Height;
			target.Srgb = Asset::IsMaterialTextureSlotSrgb(slot);
			target.Pixels.assign(embedded.Pixels.begin(), embedded.Pixels.end());
			if (logCallback)
			{
				logCallback(std::format(
					"Material embedded texture loaded - MaterialIndex={} Slot={} UploadedAs={}",
					materialIndex,
					Asset::MaterialTextureSlotName(slot),
					target.Srgb ? "sRGB RGBA8" : "linear RGBA8"));
			}
		}

		[[nodiscard]] bool HasAlphaBelowThreshold(const CpuMaterialTextureSlot& texture)
		{
			if (!texture.IsValid())
			{
				return false;
			}

			for (size_t pixelOffset = 3; pixelOffset < texture.Pixels.size(); pixelOffset += 4)
			{
				if (texture.Pixels[pixelOffset] < 250)
				{
					return true;
				}
			}

			return false;
		}

		[[nodiscard]] bool HasRedBelowThreshold(const CpuMaterialTextureSlot& texture)
		{
			if (!texture.IsValid())
			{
				return false;
			}

			for (size_t pixelOffset = 0; pixelOffset < texture.Pixels.size(); pixelOffset += 4)
			{
				if (texture.Pixels[pixelOffset] < 250)
				{
					return true;
				}
			}

			return false;
		}
	}

	bool MaterialTextureSystem::HasTransparency(const CpuMaterialTexture& materialTexture)
	{
		return HasAlphaBelowThreshold(materialTexture.Slot(Asset::MaterialTextureSlot::BaseColor))
			|| HasRedBelowThreshold(materialTexture.Slot(Asset::MaterialTextureSlot::Opacity));
	}

	bool MaterialTextureSystem::LoadCpuMaterialTextures(
		Scene& scene,
		SceneRenderState& renderState,
		EntityId entityId,
		const LogCallback& logCallback)
	{
		Asset::StaticMeshAsset* meshAsset = scene.GetMeshAsset(entityId);
		std::vector<CpuMaterialTexture>* materialTextures = scene.GetMaterialTextures(entityId);
		if (!materialTextures)
		{
			return false;
		}

		materialTextures->clear();

		if (!meshAsset)
		{
			renderState.PrimaryMaterialTransparency.clear();
			if (entityId != InvalidEntityId)
			{
				renderState.EntityMaterialTransparency[entityId].clear();
			}
			return true;
		}

		if (!LoadCpuMaterialTextures(*meshAsset, *materialTextures, &renderState.PrimaryMaterialTransparency, logCallback))
		{
			return false;
		}

		if (logCallback)
		{
			std::string materialTextureSummaryLogMessage = "Material texture count=";
			materialTextureSummaryLogMessage.append(std::to_string(materialTextures->size()));
			logCallback(materialTextureSummaryLogMessage);
		}

		if (entityId != InvalidEntityId)
		{
			renderState.EntityMaterialTransparency[entityId] = renderState.PrimaryMaterialTransparency;
		}

		return true;
	}

	bool MaterialTextureSystem::LoadCpuMaterialTextures(
		const Asset::StaticMeshAsset& meshAsset,
		std::vector<CpuMaterialTexture>& materialTextures,
		std::vector<bool>* materialTransparency,
		const LogCallback& logCallback)
	{
		materialTextures.clear();

		const size_t textureCount = (std::max)(static_cast<size_t>(1), meshAsset.Materials.size());
		materialTextures.resize(textureCount);
		if (materialTransparency)
		{
			materialTransparency->assign(textureCount, false);
		}

		for (size_t materialIndex = 0; materialIndex < textureCount; ++materialIndex)
		{
			auto& materialTexture = materialTextures[materialIndex];
			const Asset::StaticMeshMaterial* material = materialIndex < meshAsset.Materials.size() ? &meshAsset.Materials[materialIndex] : nullptr;
			SetSlotFallback(materialTexture, material);
			if (materialTransparency && materialIndex < materialTransparency->size())
			{
				(*materialTransparency)[materialIndex] = HasTransparency(materialTexture);
			}

			if (!material)
			{
				continue;
			}

			for (size_t slotIndex = 0; slotIndex < Asset::kMaterialTextureSlotCount; ++slotIndex)
			{
				const auto slot = static_cast<Asset::MaterialTextureSlot>(slotIndex);
				const Asset::MaterialTextureBinding& binding = material->TextureBindings[slotIndex];
				CpuMaterialTextureSlot& slotTexture = materialTexture.Slots[slotIndex];
				std::filesystem::path legacyPath;
				if (slot == Asset::MaterialTextureSlot::BaseColor)
				{
					legacyPath = material->DiffuseTexturePath;
				}
				else if (slot == Asset::MaterialTextureSlot::Normal)
				{
					legacyPath = material->NormalTexturePath;
				}
				else if (slot == Asset::MaterialTextureSlot::MetallicRoughness)
				{
					legacyPath = material->MetallicRoughnessTexturePath;
				}

				if (!binding.Path.empty())
				{
					static_cast<void>(LoadTextureFile(slotTexture, binding.Path, slot, materialIndex, logCallback));
				}
				else if (!legacyPath.empty())
				{
					static_cast<void>(LoadTextureFile(slotTexture, legacyPath, slot, materialIndex, logCallback));
				}
				else if (binding.Embedded.IsValid())
				{
					LoadEmbeddedTexture(slotTexture, binding.Embedded, slot, materialIndex, logCallback);
				}

				if (binding.Path.empty()
					&& !binding.Embedded.IsValid()
					&& slot == Asset::MaterialTextureSlot::BaseColor
					&& !material->EmbeddedDiffuseTexturePixels.empty()
					&& material->EmbeddedDiffuseTextureWidth > 0
					&& material->EmbeddedDiffuseTextureHeight > 0)
				{
					Asset::EmbeddedMaterialTexture embedded;
					embedded.Width = material->EmbeddedDiffuseTextureWidth;
					embedded.Height = material->EmbeddedDiffuseTextureHeight;
					embedded.Pixels.assign(material->EmbeddedDiffuseTexturePixels.begin(), material->EmbeddedDiffuseTexturePixels.end());
					LoadEmbeddedTexture(slotTexture, embedded, slot, materialIndex, logCallback);
				}
			}

			if (materialTransparency && materialIndex < materialTransparency->size())
			{
				(*materialTransparency)[materialIndex] = HasTransparency(materialTexture);
			}

			if (logCallback)
			{
				logCallback(std::format(
					"Material shading - MaterialIndex={} Name={} Model={} Base={} Normal={} MR={} Specular={}",
					materialIndex,
					material->Name.empty() ? "<unnamed>" : material->Name,
					Asset::MaterialShadingModelName(material->ShadingModel),
					materialTexture.Slot(Asset::MaterialTextureSlot::BaseColor).Path.empty() ? "<fallback>" : materialTexture.Slot(Asset::MaterialTextureSlot::BaseColor).Path.string(),
					materialTexture.Slot(Asset::MaterialTextureSlot::Normal).Path.empty() ? "<fallback>" : materialTexture.Slot(Asset::MaterialTextureSlot::Normal).Path.string(),
					materialTexture.Slot(Asset::MaterialTextureSlot::MetallicRoughness).Path.empty() ? "<fallback>" : materialTexture.Slot(Asset::MaterialTextureSlot::MetallicRoughness).Path.string(),
					materialTexture.Slot(Asset::MaterialTextureSlot::Specular).Path.empty() ? "<fallback>" : materialTexture.Slot(Asset::MaterialTextureSlot::Specular).Path.string()));
			}
		}

		return true;
	}
}
