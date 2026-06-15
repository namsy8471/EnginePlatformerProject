#pragma once

#include "StaticMesh.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace Asset
{
	struct ModelInspectionSummary
	{
		bool HasScene = false;
		bool HasRootNode = false;
		bool IsIncomplete = false;
		bool HasAnimations = false;
		uint32_t MeshCount = 0;
		uint32_t MaterialCount = 0;
		uint32_t AnimationCount = 0;
		uint32_t RenderableMeshCount = 0;
		uint64_t VertexCount = 0;
		uint64_t FaceCount = 0;
		uint64_t IndexCount = 0;
		std::string AssimpError;
	};

	class AssimpModelLoader
	{
	public:
		[[nodiscard]] ModelInspectionSummary InspectModel(std::string_view filePath) const;
		[[nodiscard]] bool HasAnimation(std::string_view filePath) const;
		[[nodiscard]] std::unique_ptr<StaticMeshAsset> LoadAnimatedMesh(std::string_view filePath) const;
		[[nodiscard]] std::unique_ptr<StaticMeshAsset> LoadStaticMesh(std::string_view filePath) const;
	};
}
