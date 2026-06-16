#include "Materials/ShaderVariant.h"

#include <algorithm>
#include <format>
#include <utility>

namespace Materials
{
	namespace
	{
		[[nodiscard]] constexpr uint32_t ToValue(GraphicsAPI api) noexcept
		{
			return static_cast<uint32_t>(api);
		}

		[[nodiscard]] constexpr uint32_t ToValue(RenderMode mode) noexcept
		{
			return static_cast<uint32_t>(mode);
		}

		[[nodiscard]] constexpr uint32_t ToValue(Asset::MaterialShadingModel model) noexcept
		{
			return static_cast<uint32_t>(model);
		}

		[[nodiscard]] constexpr uint32_t ToValue(MaterialDebugView view) noexcept
		{
			return static_cast<uint32_t>(view);
		}

		[[nodiscard]] constexpr uint32_t ToValue(ShaderFeatureFlags flags) noexcept
		{
			return static_cast<uint32_t>(flags);
		}

		[[nodiscard]] const char* GraphicsApiName(GraphicsAPI api) noexcept
		{
			switch (api)
			{
			case GraphicsAPI::DirectX12:
				return "DX12";
			case GraphicsAPI::Vulkan:
			default:
				return "Vulkan";
			}
		}

		[[nodiscard]] const char* DebugViewName(MaterialDebugView view) noexcept
		{
			switch (view)
			{
			case MaterialDebugView::BaseColor:
				return "BaseColor";
			case MaterialDebugView::Normal:
				return "Normal";
			case MaterialDebugView::Metallic:
				return "Metallic";
			case MaterialDebugView::Roughness:
				return "Roughness";
			case MaterialDebugView::AO:
				return "AO";
			case MaterialDebugView::Emissive:
				return "Emissive";
			case MaterialDebugView::LightingOnly:
				return "LightingOnly";
			case MaterialDebugView::VertexColor:
				return "VertexColor";
			case MaterialDebugView::Shadow:
				return "Shadow";
			case MaterialDebugView::DeferredTileLights:
				return "TileLights";
			case MaterialDebugView::Lit:
			default:
				return "Lit";
			}
		}

		void AddDefineIf(std::vector<std::string>& defines, bool condition, std::string define)
		{
			if (condition)
			{
				defines.push_back(std::move(define));
			}
		}
	}

	size_t ShaderVariantKeyHash::operator()(const ShaderVariantKey& key) const noexcept
	{
		size_t hash = 1469598103934665603ull;
		const auto mix = [&hash](uint32_t value)
		{
			hash ^= value;
			hash *= 1099511628211ull;
		};
		mix(ToValue(key.Api));
		mix(ToValue(key.Mode));
		mix(ToValue(key.ShadingModel));
		mix(ToValue(key.DebugView));
		mix(ToValue(key.Features));
		return hash;
	}

	const ShaderVariantRecord& ShaderVariantCache::GetOrCreate(const ShaderVariantKey& key)
	{
		auto [it, inserted] = m_Records.try_emplace(key);
		ShaderVariantRecord& record = it->second;
		if (inserted)
		{
			record.Key = key;
			record.Name = ToString(key);
			record.Defines = BuildShaderDefines(key);
		}
		++record.RequestCount;
		return record;
	}

	void ShaderVariantCache::Clear()
	{
		m_Records.clear();
	}

	ShaderVariantCacheStats ShaderVariantCache::GetStats() const
	{
		ShaderVariantCacheStats stats;
		stats.VariantCount = m_Records.size();
		for (const auto& [key, record] : m_Records)
		{
			(void)key;
			stats.RequestCount += record.RequestCount;
			switch (record.Key.ShadingModel)
			{
			case Asset::MaterialShadingModel::PBR:
				++stats.PbrVariantCount;
				break;
			case Asset::MaterialShadingModel::Unlit:
				++stats.UnlitVariantCount;
				break;
			case Asset::MaterialShadingModel::Phong:
			default:
				++stats.PhongVariantCount;
				break;
			}
			if (record.Key.Mode == RenderMode::Deferred)
			{
				++stats.DeferredVariantCount;
			}
			if (HasFlag(record.Key.Features, ShaderFeatureFlags::Transparent))
			{
				++stats.TransparentVariantCount;
			}
		}
		return stats;
	}

	std::vector<ShaderVariantRecord> ShaderVariantCache::GetRecordsSnapshot() const
	{
		std::vector<ShaderVariantRecord> records;
		records.reserve(m_Records.size());
		for (const auto& [key, record] : m_Records)
		{
			(void)key;
			records.push_back(record);
		}
		std::ranges::sort(records, [](const ShaderVariantRecord& lhs, const ShaderVariantRecord& rhs)
			{
				return lhs.Name < rhs.Name;
			});
		return records;
	}

	ShaderFeatureFlags BuildShaderFeatureFlags(
		const Asset::StaticMeshMaterial& material,
		bool transparent,
		bool skinned,
		bool instanced,
		bool deferredGeometry,
		bool deferredLighting)
	{
		ShaderFeatureFlags flags = ShaderFeatureFlags::None;
		const auto hasTexture = [&material](Asset::MaterialTextureSlot slot)
		{
			return Asset::GetMaterialTextureBinding(material, slot).HasSource()
				|| !Asset::GetMaterialTexturePath(material, slot).empty();
		};

		if (hasTexture(Asset::MaterialTextureSlot::Normal))
		{
			flags |= ShaderFeatureFlags::NormalMap;
		}
		if (hasTexture(Asset::MaterialTextureSlot::Opacity))
		{
			flags |= ShaderFeatureFlags::OpacityMap;
		}
		if (hasTexture(Asset::MaterialTextureSlot::Emissive))
		{
			flags |= ShaderFeatureFlags::EmissiveMap;
		}
		if (hasTexture(Asset::MaterialTextureSlot::MetallicRoughness)
			|| hasTexture(Asset::MaterialTextureSlot::Metallic)
			|| hasTexture(Asset::MaterialTextureSlot::Roughness)
			|| hasTexture(Asset::MaterialTextureSlot::AO))
		{
			flags |= ShaderFeatureFlags::MetallicRoughnessMap;
		}
		if (hasTexture(Asset::MaterialTextureSlot::Specular)
			|| hasTexture(Asset::MaterialTextureSlot::Shininess))
		{
			flags |= ShaderFeatureFlags::SpecularMap;
		}
		if (material.UseVertexColor)
		{
			flags |= ShaderFeatureFlags::UseVertexColor;
		}
		if (material.NormalYFlip)
		{
			flags |= ShaderFeatureFlags::NormalYFlip;
		}
		if (skinned)
		{
			flags |= ShaderFeatureFlags::Skinned;
		}
		if (instanced)
		{
			flags |= ShaderFeatureFlags::Instanced;
		}
		if (transparent)
		{
			flags |= ShaderFeatureFlags::Transparent;
		}
		if (deferredGeometry)
		{
			flags |= ShaderFeatureFlags::DeferredGeometry;
		}
		if (deferredLighting)
		{
			flags |= ShaderFeatureFlags::DeferredLighting;
		}
		return flags;
	}

	ShaderVariantKey BuildShaderVariantKey(
		GraphicsAPI api,
		RenderMode mode,
		const Asset::StaticMeshMaterial& material,
		MaterialDebugView debugView,
		bool transparent,
		bool skinned,
		bool instanced,
		bool deferredGeometry,
		bool deferredLighting)
	{
		return ShaderVariantKey{
			.Api = api,
			.Mode = mode,
			.ShadingModel = material.ShadingModel,
			.DebugView = debugView,
			.Features = BuildShaderFeatureFlags(material, transparent, skinned, instanced, deferredGeometry, deferredLighting)
		};
	}

	std::string ToString(const ShaderVariantKey& key)
	{
		return std::format(
			"{}|{}|{}|{}|0x{:08X}",
			GraphicsApiName(key.Api),
			RenderModeToString(key.Mode),
			Asset::MaterialShadingModelName(key.ShadingModel),
			DebugViewName(key.DebugView),
			ToValue(key.Features));
	}

	std::vector<std::string> BuildShaderDefines(const ShaderVariantKey& key)
	{
		std::vector<std::string> defines;
		defines.push_back(std::format("ENGINE_GRAPHICS_API_{}", GraphicsApiName(key.Api)));
		defines.push_back(std::format("ENGINE_RENDER_MODE_{}", RenderModeToString(key.Mode)));
		defines.push_back(std::format("ENGINE_MATERIAL_{}", Asset::MaterialShadingModelName(key.ShadingModel)));
		defines.push_back(std::format("ENGINE_DEBUG_VIEW_{}", DebugViewName(key.DebugView)));
		AddDefineIf(defines, HasFlag(key.Features, ShaderFeatureFlags::NormalMap), "ENGINE_HAS_NORMAL_MAP");
		AddDefineIf(defines, HasFlag(key.Features, ShaderFeatureFlags::OpacityMap), "ENGINE_HAS_OPACITY_MAP");
		AddDefineIf(defines, HasFlag(key.Features, ShaderFeatureFlags::EmissiveMap), "ENGINE_HAS_EMISSIVE_MAP");
		AddDefineIf(defines, HasFlag(key.Features, ShaderFeatureFlags::MetallicRoughnessMap), "ENGINE_HAS_METALLIC_ROUGHNESS_MAP");
		AddDefineIf(defines, HasFlag(key.Features, ShaderFeatureFlags::SpecularMap), "ENGINE_HAS_SPECULAR_MAP");
		AddDefineIf(defines, HasFlag(key.Features, ShaderFeatureFlags::UseVertexColor), "ENGINE_USE_VERTEX_COLOR");
		AddDefineIf(defines, HasFlag(key.Features, ShaderFeatureFlags::NormalYFlip), "ENGINE_NORMAL_Y_FLIP");
		AddDefineIf(defines, HasFlag(key.Features, ShaderFeatureFlags::Skinned), "ENGINE_SKINNED");
		AddDefineIf(defines, HasFlag(key.Features, ShaderFeatureFlags::Instanced), "ENGINE_INSTANCED");
		AddDefineIf(defines, HasFlag(key.Features, ShaderFeatureFlags::Transparent), "ENGINE_TRANSPARENT");
		AddDefineIf(defines, HasFlag(key.Features, ShaderFeatureFlags::DeferredGeometry), "ENGINE_DEFERRED_GEOMETRY");
		AddDefineIf(defines, HasFlag(key.Features, ShaderFeatureFlags::DeferredLighting), "ENGINE_DEFERRED_LIGHTING");
		return defines;
	}
}
