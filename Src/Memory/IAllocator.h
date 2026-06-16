#pragma once

#include "Memory/MemoryTypes.h"

#include <cstddef>

namespace Memory
{
	class IAllocator
	{
	public:
		virtual ~IAllocator() = default;

		[[nodiscard]] virtual void* Allocate(size_t size, size_t alignment, MemoryTag tag) = 0;
		virtual void Free(void* pointer) noexcept = 0;
		[[nodiscard]] virtual MemorySystemStats GetStats() const = 0;
	};
}
