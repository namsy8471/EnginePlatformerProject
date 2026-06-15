#pragma once

#include <Windows.h>

#include <cstdint>
#include <format>
#include <source_location>
#include <stdexcept>

[[nodiscard]] inline bool Failed(HRESULT result) noexcept
{
	return FAILED(result);
}

inline void ThrowIfFailed(HRESULT result, const std::source_location& location = std::source_location::current())
{
	if (!Failed(result))
	{
		return;
	}

	throw std::runtime_error(std::format(
		"DX12 error 0x{:08X} at {}:{}",
		static_cast<uint32_t>(result),
		location.file_name(),
		location.line()));
}
