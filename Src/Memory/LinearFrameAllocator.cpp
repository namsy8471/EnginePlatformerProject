#include "Memory/LinearFrameAllocator.h"

#include <algorithm>
#include <new>

namespace Memory
{
	namespace
	{
		[[nodiscard]] size_t NormalizeAlignment(size_t alignment) noexcept
		{
			return (std::max)(alignment, alignof(std::max_align_t));
		}

		[[nodiscard]] size_t AlignUp(size_t value, size_t alignment) noexcept
		{
			const size_t mask = alignment - 1;
			return (value + mask) & ~mask;
		}
	}

	void LinearFrameAllocator::Initialize(size_t capacityBytes)
	{
		m_Buffer = std::make_unique<std::byte[]>(capacityBytes);
		m_Capacity = capacityBytes;
		m_Offset = 0;
		m_PeakOffset = 0;
		m_TotalAllocatedBytes = 0;
		m_AllocationCount = 0;
	}

	void LinearFrameAllocator::Shutdown() noexcept
	{
		m_Buffer.reset();
		m_Capacity = 0;
		m_Offset = 0;
		m_PeakOffset = 0;
		m_TotalAllocatedBytes = 0;
		m_AllocationCount = 0;
	}

	void* LinearFrameAllocator::Allocate(size_t size, size_t alignment)
	{
		const size_t requestedSize = (std::max)(size, size_t{ 1 });
		const size_t requestedAlignment = NormalizeAlignment(alignment);
		const size_t alignedOffset = AlignUp(m_Offset, requestedAlignment);
		if (!m_Buffer || alignedOffset + requestedSize > m_Capacity)
		{
			throw std::bad_alloc();
		}

		void* result = m_Buffer.get() + alignedOffset;
		m_Offset = alignedOffset + requestedSize;
		m_PeakOffset = (std::max)(m_PeakOffset, m_Offset);
		m_TotalAllocatedBytes += requestedSize;
		++m_AllocationCount;
		return result;
	}

	void LinearFrameAllocator::Reset() noexcept
	{
		m_Offset = 0;
	}

	bool LinearFrameAllocator::Contains(const void* pointer) const noexcept
	{
		const auto* bytePointer = static_cast<const std::byte*>(pointer);
		return m_Buffer
			&& bytePointer >= m_Buffer.get()
			&& bytePointer < m_Buffer.get() + m_Capacity;
	}

	MemoryStats LinearFrameAllocator::GetStats() const noexcept
	{
		return MemoryStats{
			.CurrentBytes = m_Offset,
			.PeakBytes = m_PeakOffset,
			.TotalAllocatedBytes = m_TotalAllocatedBytes,
			.AllocationCount = m_AllocationCount,
			.FreeCount = 0
		};
	}
}
