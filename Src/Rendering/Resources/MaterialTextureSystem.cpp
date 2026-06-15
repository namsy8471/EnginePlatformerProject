#include "MaterialTextureSystem.h"

#include "Scene/SceneRenderState.h"

#include <stb_image.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace Rendering
{
	bool MaterialTextureSystem::HasTransparency(const CpuMaterialTexture& materialTexture)
	{
		if (materialTexture.Width <= 0 || materialTexture.Height <= 0)
		{
			return false;
		}

		for (size_t pixelOffset = 3; pixelOffset < materialTexture.Pixels.size(); pixelOffset += 4)
		{
			if (materialTexture.Pixels[pixelOffset] < 250)
			{
				return true;
			}
		}

		return false;
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
			materialTexture.Path.clear();
			materialTexture.Pixels = { 255, 255, 255, 255 };
			materialTexture.Width = 1;
			materialTexture.Height = 1;

			if (materialIndex >= meshAsset.Materials.size())
			{
				continue;
			}

			const auto& material = meshAsset.Materials[materialIndex];
			if (!material.DiffuseTexturePath.empty() && std::filesystem::exists(material.DiffuseTexturePath))
			{
				int width = 0;
				int height = 0;
				int channels = 0;
				stbi_uc* pixels = stbi_load(material.DiffuseTexturePath.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
				if (!pixels)
				{
					if (logCallback)
					{
						std::string failureLogMessage = "Material texture load failed - MaterialIndex=";
						failureLogMessage.append(std::to_string(materialIndex));
						failureLogMessage.append(" Path=");
						failureLogMessage.append(material.DiffuseTexturePath.string());
						failureLogMessage.append(" SelectedPath=<fallback white>");
						logCallback(failureLogMessage);
					}
					continue;
				}

				materialTexture.Path = material.DiffuseTexturePath;
				materialTexture.Width = width;
				materialTexture.Height = height;
				materialTexture.Pixels.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
				stbi_image_free(pixels);
				if (materialTransparency && materialIndex < materialTransparency->size())
				{
					(*materialTransparency)[materialIndex] = HasTransparency(materialTexture);
				}

				if (logCallback)
				{
					std::string loadedTextureLogMessage = "Material texture loaded - MaterialIndex=";
					loadedTextureLogMessage.append(std::to_string(materialIndex));
					loadedTextureLogMessage.append(" Path=");
					loadedTextureLogMessage.append(materialTexture.Path.string());
					loadedTextureLogMessage.append(" | SourceChannels=");
					loadedTextureLogMessage.append(std::to_string(channels));
					loadedTextureLogMessage.append(" | UploadedAs=sRGB RGBA8");
					logCallback(loadedTextureLogMessage);
				}
				continue;
			}

			if (!material.EmbeddedDiffuseTexturePixels.empty() && material.EmbeddedDiffuseTextureWidth > 0 && material.EmbeddedDiffuseTextureHeight > 0)
			{
				materialTexture.Width = material.EmbeddedDiffuseTextureWidth;
				materialTexture.Height = material.EmbeddedDiffuseTextureHeight;
				materialTexture.Pixels = material.EmbeddedDiffuseTexturePixels;
				if (materialTransparency && materialIndex < materialTransparency->size())
				{
					(*materialTransparency)[materialIndex] = HasTransparency(materialTexture);
				}

				if (logCallback)
				{
					std::string embeddedTextureLogMessage = "Material embedded texture loaded - MaterialIndex=";
					embeddedTextureLogMessage.append(std::to_string(materialIndex));
					embeddedTextureLogMessage.append(" Path=<embedded>");
					embeddedTextureLogMessage.append(" | UploadedAs=sRGB RGBA8");
					logCallback(embeddedTextureLogMessage);
				}
				continue;
			}

			if (material.DiffuseTexturePath.empty() || !std::filesystem::exists(material.DiffuseTexturePath))
			{
				if (logCallback)
				{
					std::string fallbackLogMessage = "Material texture fallback - MaterialIndex=";
					fallbackLogMessage.append(std::to_string(materialIndex));
					fallbackLogMessage.append(" SelectedPath=<fallback white>");
					logCallback(fallbackLogMessage);
				}
				continue;
			}
		}

		return true;
	}
}
