#pragma once

#include "Memory/MemoryTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace Memory
{
	class LinearFrameAllocator
	{
	public:
		LinearFrameAllocator() = default;
		~LinearFrameAllocator() = default;

		LinearFrameAllocator(const LinearFrameAllocator&) = delete;
		LinearFrameAllocator& operator=(const LinearFrameAllocator&) = delete;

		void Initialize(size_t capacityBytes);
		void Shutdown() noexcept;
		[[nodiscard]] void* Allocate(size_t size, size_t alignment);
		void Reset() noexcept;
		[[nodiscard]] bool Contains(const void* pointer) const noexcept;
		[[nodiscard]] MemoryStats GetStats() const noexcept;
		[[nodiscard]] size_t GetCapacity() const noexcept { return m_Capacity; }
		[[nodiscard]] size_t GetCurrentOffset() const noexcept { return m_Offset; }
		[[nodiscard]] size_t GetPeakOffset() const noexcept { return m_PeakOffset; }

	private:
		std::unique_ptr<std::byte[]> m_Buffer;
		size_t m_Capacity = 0;
		size_t m_Offset = 0;
		size_t m_PeakOffset = 0;
		size_t m_TotalAllocatedBytes = 0;
		size_t m_AllocationCount = 0;
	};
}
