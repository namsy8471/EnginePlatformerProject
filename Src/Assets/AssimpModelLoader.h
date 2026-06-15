#pragma once

#include "StaticMesh.h"

#include <memory>
#include <string_view>

namespace Asset
{
	class AssimpModelLoader
	{
	public:
		[[nodiscard]] bool HasAnimation(std::string_view filePath) const;
		[[nodiscard]] std::unique_ptr<StaticMeshAsset> LoadAnimatedMesh(std::string_view filePath) const;
		[[nodiscard]] std::unique_ptr<StaticMeshAsset> LoadStaticMesh(std::string_view filePath) const;
	};
}
