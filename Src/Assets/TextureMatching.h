#pragma once

#include "StaticMesh.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Asset::TextureMatching
{
	inline constexpr std::array<std::string_view, 9> kBaseColorKeywords = {
		"basecolor", "base_color", "albedo", "diffuse", "diff", "color", "colour", "body", "tex_color"
	};
	inline constexpr std::array<std::string_view, 6> kNormalKeywords = {
		"normal", "nrm", "norm", "_n.", "bump", "height"
	};
	inline constexpr std::array<std::string_view, 5> kMetallicKeywords = {
		"metallic", "metalness", "_metal", "metal", "_m."
	};
	inline constexpr std::array<std::string_view, 5> kRoughnessKeywords = {
		"roughness", "_rough", "_rgh", "rough", "_r."
	};
	inline constexpr std::array<std::string_view, 9> kMetallicRoughnessKeywords = {
		"metallicroughness", "metallic_roughness", "metal_rough", "metalrough",
		"orm", "mrao", "rma", "arm", "occlusionroughnessmetallic"
	};
	inline constexpr std::array<std::string_view, 6> kAoKeywords = {
		"ambientocclusion", "ambient_occlusion", "occlusion", "_ao", "ao_", "-ao"
	};
	inline constexpr std::array<std::string_view, 4> kEmissiveKeywords = {
		"emissive", "emission", "glow", "emit"
	};
	inline constexpr std::array<std::string_view, 4> kOpacityKeywords = {
		"opacity", "alpha", "transparency", "transparent"
	};
	inline constexpr std::array<std::string_view, 4> kSpecularKeywords = {
		"specular", "_spec", "spec_", "reflection"
	};
	inline constexpr std::array<std::string_view, 5> kShininessKeywords = {
		"shininess", "gloss", "glossiness", "smoothness", "smooth"
	};
	inline constexpr std::array<std::string_view, 24> kNonBaseColorKeywords = {
		"ambientocclusion", "ambient_occlusion", "alpha", "ao_", "_ao", "-ao",
		"bump", "emissive", "gloss", "glow", "height", "metal", "metallic",
		"metalness", "_n.", "_nrm", "normal", "nrm", "norm", "opacity",
		"rough", "roughness", "spec", "specular"
	};
	inline constexpr std::array<std::string_view, 20> kIgnoredMatchTokens = {
		"asset", "default", "fbx", "image", "jpg", "jpeg", "material", "materials",
		"mesh", "model", "neuer", "none", "ordner", "png", "source", "tga",
		"tex", "texture", "textures", "video"
	};

	template <size_t Count>
	[[nodiscard]] constexpr std::span<const std::string_view> AsSpan(const std::array<std::string_view, Count>& values) noexcept
	{
		return std::span<const std::string_view>(values.data(), values.size());
	}

	[[nodiscard]] constexpr std::span<const std::string_view> SlotKeywords(MaterialTextureSlot slot) noexcept
	{
		switch (slot)
		{
		case MaterialTextureSlot::BaseColor:
			return AsSpan(kBaseColorKeywords);
		case MaterialTextureSlot::Normal:
			return AsSpan(kNormalKeywords);
		case MaterialTextureSlot::Metallic:
			return AsSpan(kMetallicKeywords);
		case MaterialTextureSlot::Roughness:
			return AsSpan(kRoughnessKeywords);
		case MaterialTextureSlot::MetallicRoughness:
			return AsSpan(kMetallicRoughnessKeywords);
		case MaterialTextureSlot::AO:
			return AsSpan(kAoKeywords);
		case MaterialTextureSlot::Emissive:
			return AsSpan(kEmissiveKeywords);
		case MaterialTextureSlot::Opacity:
			return AsSpan(kOpacityKeywords);
		case MaterialTextureSlot::Specular:
			return AsSpan(kSpecularKeywords);
		case MaterialTextureSlot::Shininess:
			return AsSpan(kShininessKeywords);
		case MaterialTextureSlot::Count:
		default:
			return {};
		}
	}

	[[nodiscard]] constexpr std::span<const std::string_view> NonBaseColorKeywords() noexcept
	{
		return AsSpan(kNonBaseColorKeywords);
	}

	[[nodiscard]] inline bool ContainsAnyKeyword(std::string_view text, std::span<const std::string_view> keywords)
	{
		for (std::string_view keyword : keywords)
		{
			if (!keyword.empty() && text.find(keyword) != std::string_view::npos)
			{
				return true;
			}
		}
		return false;
	}

	[[nodiscard]] inline bool IsIgnoredMatchToken(std::string_view token) noexcept
	{
		return token.size() < 2 || std::ranges::find(kIgnoredMatchTokens, token) != kIgnoredMatchTokens.end();
	}

	[[nodiscard]] inline std::string ToLowerAscii(std::string_view text)
	{
		std::string result(text);
		std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
		return result;
	}

	inline void AddUniqueToken(std::vector<std::string>& tokens, std::string token)
	{
		if (token.empty() || IsIgnoredMatchToken(token))
		{
			return;
		}

		if (std::ranges::find(tokens, token) == tokens.end())
		{
			tokens.push_back(std::move(token));
		}
	}

	inline void AppendMatchTokens(std::vector<std::string>& tokens, std::string_view text)
	{
		std::string currentToken;
		for (const char rawCharacter : ToLowerAscii(text))
		{
			const auto character = static_cast<unsigned char>(rawCharacter);
			if (std::isalnum(character))
			{
				currentToken.push_back(static_cast<char>(character));
				continue;
			}

			AddUniqueToken(tokens, std::move(currentToken));
			currentToken.clear();
		}
		AddUniqueToken(tokens, std::move(currentToken));
	}
}
