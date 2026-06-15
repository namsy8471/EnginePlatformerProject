#pragma once

#include <string_view>

enum class RenderMode
{
	Forward,
	Deferred,
	ForwardPlus
};

[[nodiscard]] constexpr std::string_view RenderModeToString(RenderMode renderMode) noexcept
{
	switch (renderMode)
	{
	case RenderMode::Forward:
		return "Forward";
	case RenderMode::Deferred:
		return "Deferred";
	case RenderMode::ForwardPlus:
		return "Forward+";
	default:
		return "Unknown";
	}
}

[[nodiscard]] constexpr std::wstring_view RenderModeToWideString(RenderMode renderMode) noexcept
{
	switch (renderMode)
	{
	case RenderMode::Forward:
		return L"Forward";
	case RenderMode::Deferred:
		return L"Deferred";
	case RenderMode::ForwardPlus:
		return L"Forward+";
	default:
		return L"Unknown";
	}
}
