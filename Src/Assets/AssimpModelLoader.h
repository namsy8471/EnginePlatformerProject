#pragma once

#include "StaticMesh.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Asset
{
	struct AssimpLoadOptions
	{
		bool GenerateTangents = true;
		bool AllowAnimatedSceneAsStatic = false;
	};

	struct ModelTextureInspection
	{
		uint32_t MaterialIndex = 0;
		std::string MaterialName;
		MaterialTextureSlot Slot = MaterialTextureSlot::Count;
		std::string SourceType;
		std::string RawPath;
		std::filesystem::path ResolvedPath;
		bool Embedded = false;
		bool Resolved = false;
		bool AutoMatched = false;
	};

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
		std::vector<ModelTextureInspection> Textures;
	};

	class AssimpModelLoader
	{
	public:
		[[nodiscard]] ModelInspectionSummary InspectModel(std::string_view filePath) const;
		[[nodiscard]] bool HasAnimation(std::string_view filePath) const;
		[[nodiscard]] std::unique_ptr<StaticMeshAsset> LoadAnimatedMesh(std::string_view filePath) const;
		[[nodiscard]] std::unique_ptr<StaticMeshAsset> LoadAnimatedMesh(std::string_view filePath, const AssimpLoadOptions& options) const;
		[[nodiscard]] std::unique_ptr<StaticMeshAsset> LoadStaticMesh(std::string_view filePath) const;
		[[nodiscard]] std::unique_ptr<StaticMeshAsset> LoadStaticMesh(std::string_view filePath, const AssimpLoadOptions& options) const;
	};
}
