#pragma once

#include "StaticMesh.h"

#include <memory>
#include <string_view>

struct aiScene;

namespace Asset
{
	class AssimpModelLoader
	{
	public:
		[[nodiscard]] bool HasAnimation(std::string_view filePath) const;
		[[nodiscard]] std::unique_ptr<StaticMeshAsset> LoadAnimatedMesh(std::string_view filePath) const;
		[[nodiscard]] std::unique_ptr<StaticMeshAsset> LoadStaticMesh(std::string_view filePath) const;

	private:
		[[nodiscard]] StaticMeshMaterial LoadMaterial(const aiScene& scene, uint32_t materialIndex, const std::filesystem::path& sourcePath) const;
	};
}
