#pragma once

#include "Assets/StaticMesh.h"

#include <memory>
#include <string_view>

namespace Asset
{
	[[nodiscard]] std::string_view PrimitiveMeshKindToString(PrimitiveMeshKind kind) noexcept;
	[[nodiscard]] PrimitiveMeshKind PrimitiveMeshKindFromString(std::string_view text) noexcept;
	[[nodiscard]] std::unique_ptr<StaticMeshAsset> CreatePrimitiveMesh(PrimitiveMeshKind kind);
}
