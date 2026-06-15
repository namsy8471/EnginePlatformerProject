#pragma once

#include "Scene/Scene.h"
#include "Rendering/Resources/MaterialTextureSystem.h"

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SceneRenderState
{
	std::vector<EntityId> RenderEntities;
	std::unordered_set<EntityId> TransparentEntities;
	std::vector<bool> PrimaryMaterialTransparency;
	std::unordered_map<EntityId, std::vector<bool>> EntityMaterialTransparency;

	void Reset()
	{
		RenderEntities.clear();
		TransparentEntities.clear();
		PrimaryMaterialTransparency.clear();
		EntityMaterialTransparency.clear();
	}

	[[nodiscard]] bool IsMaterialTransparent(const Scene& scene, EntityId entityId, size_t materialIndex) const
	{
		if (TransparentEntities.find(entityId) != TransparentEntities.end())
		{
			return true;
		}

		if (entityId == scene.GetPrimaryRenderableEntity())
		{
			if (materialIndex < PrimaryMaterialTransparency.size())
			{
				return PrimaryMaterialTransparency[materialIndex];
			}
			return false;
		}

		const auto* materialTextures = scene.GetMaterialTextures(entityId);
		if (!materialTextures || materialIndex >= materialTextures->size())
		{
			return false;
		}

		const auto cachedTransparencyIt = EntityMaterialTransparency.find(entityId);
		if (cachedTransparencyIt != EntityMaterialTransparency.end())
		{
			const auto& cachedTransparency = cachedTransparencyIt->second;
			if (materialIndex < cachedTransparency.size())
			{
				return cachedTransparency[materialIndex];
			}
			return false;
		}

		return Rendering::MaterialTextureSystem::HasTransparency((*materialTextures)[materialIndex]);
	}
};
