#pragma once

#include "Assets/StaticMesh.h"
#include "Rendering/RHI/GraphicsCommon.h"
#include "Rendering/RenderMode.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Materials
{
	enum class ShaderFeatureFlags : uint32_t
	{
		None = 0,
		NormalMap = 1u << 0,
		OpacityMap = 1u << 1,
		EmissiveMap = 1u << 2,
		MetallicRoughnessMap = 1u << 3,
		SpecularMap = 1u << 4,
		UseVertexColor = 1u << 5,
		NormalYFlip = 1u << 6,
		Skinned = 1u << 7,
		Instanced = 1u << 8,
		Transparent = 1u << 9,
		DeferredGeometry = 1u << 10,
		DeferredLighting = 1u << 11
	};

	[[nodiscard]] constexpr ShaderFeatureFlags operator|(ShaderFeatureFlags lhs, ShaderFeatureFlags rhs) noexcept
	{
		return static_cast<ShaderFeatureFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
	}

	constexpr ShaderFeatureFlags& operator|=(ShaderFeatureFlags& lhs, ShaderFeatureFlags rhs) noexcept
	{
		lhs = lhs | rhs;
		return lhs;
	}

	[[nodiscard]] constexpr bool HasFlag(ShaderFeatureFlags flags, ShaderFeatureFlags flag) noexcept
	{
		return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
	}

	struct ShaderVariantKey
	{
		GraphicsAPI Api = GraphicsAPI::Vulkan;
		RenderMode Mode = RenderMode::Forward;
		Asset::MaterialShadingModel ShadingModel = Asset::MaterialShadingModel::Phong;
		MaterialDebugView DebugView = MaterialDebugView::Lit;
		ShaderFeatureFlags Features = ShaderFeatureFlags::None;

		[[nodiscard]] friend bool operator==(const ShaderVariantKey& lhs, const ShaderVariantKey& rhs) noexcept
		{
			return lhs.Api == rhs.Api
				&& lhs.Mode == rhs.Mode
				&& lhs.ShadingModel == rhs.ShadingModel
				&& lhs.DebugView == rhs.DebugView
				&& lhs.Features == rhs.Features;
		}
	};

	struct ShaderVariantKeyHash
	{
		[[nodiscard]] size_t operator()(const ShaderVariantKey& key) const noexcept;
	};

	struct ShaderVariantRecord
	{
		ShaderVariantKey Key;
		std::string Name;
		std::vector<std::string> Defines;
		uint64_t RequestCount = 0;
	};

	struct ShaderVariantCacheStats
	{
		size_t VariantCount = 0;
		uint64_t RequestCount = 0;
		size_t PbrVariantCount = 0;
		size_t PhongVariantCount = 0;
		size_t UnlitVariantCount = 0;
		size_t DeferredVariantCount = 0;
		size_t TransparentVariantCount = 0;
	};

	class ShaderVariantCache
	{
	public:
		[[nodiscard]] const ShaderVariantRecord& GetOrCreate(const ShaderVariantKey& key);
		void Clear();
		[[nodiscard]] ShaderVariantCacheStats GetStats() const;
		[[nodiscard]] std::vector<ShaderVariantRecord> GetRecordsSnapshot() const;

	private:
		std::unordered_map<ShaderVariantKey, ShaderVariantRecord, ShaderVariantKeyHash> m_Records;
	};

	[[nodiscard]] ShaderFeatureFlags BuildShaderFeatureFlags(
		const Asset::StaticMeshMaterial& material,
		bool transparent,
		bool skinned,
		bool instanced,
		bool deferredGeometry,
		bool deferredLighting);

	[[nodiscard]] ShaderVariantKey BuildShaderVariantKey(
		GraphicsAPI api,
		RenderMode mode,
		const Asset::StaticMeshMaterial& material,
		MaterialDebugView debugView,
		bool transparent,
		bool skinned = false,
		bool instanced = false,
		bool deferredGeometry = false,
		bool deferredLighting = false);

	[[nodiscard]] std::string ToString(const ShaderVariantKey& key);
	[[nodiscard]] std::vector<std::string> BuildShaderDefines(const ShaderVariantKey& key);
}
