#include "Materials/MaterialResource.h"

namespace Materials
{
	MaterialResource BuildMaterialResource(
		const Asset::StaticMeshMaterial& material,
		Resources::ResourceHandle handle)
	{
		MaterialResource resource;
		resource.Handle = handle;
		resource.Name = material.Name;
		resource.ShadingModel = material.ShadingModel;
		resource.BaseColor = material.DiffuseColor;
		resource.ImportedDiffuseTint = material.ImportedDiffuseTint;
		resource.SpecularColor = material.SpecularColor;
		resource.EmissiveColor = material.EmissiveColor;
		resource.MetallicFactor = material.MetallicFactor;
		resource.RoughnessFactor = material.RoughnessFactor;
		resource.Shininess = material.Shininess;
		resource.Opacity = material.Opacity;
		resource.UseVertexColor = material.UseVertexColor;
		resource.NormalYFlip = material.NormalYFlip;

		for (size_t slotIndex = 0; slotIndex < Asset::kMaterialTextureSlotCount; ++slotIndex)
		{
			const auto slot = static_cast<Asset::MaterialTextureSlot>(slotIndex);
			const Asset::MaterialTextureBinding& binding = material.TextureBindings[slotIndex];
			resource.TextureSlots[slotIndex] = MaterialTextureSlotResource{
				.Slot = slot,
				.SourcePath = Asset::GetMaterialTexturePath(material, slot),
				.IsOverride = binding.IsOverride,
				.IsEmbedded = binding.Embedded.IsValid(),
				.Srgb = Asset::IsMaterialTextureSlotSrgb(slot)
			};
		}
		return resource;
	}

	std::vector<MaterialResource> BuildMaterialResources(const Asset::StaticMeshAsset& meshAsset)
	{
		std::vector<MaterialResource> materials;
		materials.reserve(meshAsset.Materials.size());
		for (const Asset::StaticMeshMaterial& material : meshAsset.Materials)
		{
			materials.push_back(BuildMaterialResource(material));
		}
		return materials;
	}

	MaterialResourceStats BuildMaterialResourceStats(const std::vector<MaterialResource>& materials)
	{
		MaterialResourceStats stats;
		stats.MaterialCount = materials.size();
		for (const MaterialResource& material : materials)
		{
			switch (material.ShadingModel)
			{
			case Asset::MaterialShadingModel::PBR:
				++stats.PbrCount;
				break;
			case Asset::MaterialShadingModel::Unlit:
				++stats.UnlitCount;
				break;
			case Asset::MaterialShadingModel::Phong:
			default:
				++stats.PhongCount;
				break;
			}

			for (const MaterialTextureSlotResource& slot : material.TextureSlots)
			{
				if (!slot.SourcePath.empty() || slot.IsEmbedded)
				{
					++stats.TextureSlotCount;
				}
				if (slot.IsOverride)
				{
					++stats.OverrideSlotCount;
				}
				if (slot.IsEmbedded)
				{
					++stats.EmbeddedSlotCount;
				}
			}
		}
		return stats;
	}
}
