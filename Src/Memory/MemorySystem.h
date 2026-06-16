#pragma once

#include "Memory/LinearFrameAllocator.h"
#include "Memory/MemoryTypes.h"
#include "Memory/TrackedAllocator.h"

#include <cstddef>
#include <span>

namespace Memory
{
	void Initialize(const MemoryConfig& config = {});
	void Shutdown();
	void BeginFrame();
	void EndFrame();

	[[nodiscard]] void* Allocate(size_t size, size_t alignment, MemoryTag tag);
	void Free(void* pointer) noexcept;
	[[nodiscard]] MemorySystemStats GetStats();
	void DumpLeaks();
	[[nodiscard]] IAllocator& GetAllocator(MemoryTag tag);
	[[nodiscard]] std::span<const MemoryStats, kMemoryTagCount> GetTagStats();
}
