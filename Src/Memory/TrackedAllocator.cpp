#include "Memory/TrackedAllocator.h"

#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <new>

namespace Memory
{
	namespace
	{
		[[nodiscard]] size_t NormalizeAlignment(size_t alignment) noexcept
		{
			return (std::max)(alignment, alignof(std::max_align_t));
		}

		[[nodiscard]] uintptr_t AlignUp(uintptr_t value, size_t alignment) noexcept
		{
			const uintptr_t mask = static_cast<uintptr_t>(alignment - 1);
			return (value + mask) & ~mask;
		}
	}

	void* TrackedAllocator::Allocate(size_t size, size_t alignment, MemoryTag tag)
	{
		const size_t requestedSize = (std::max)(size, size_t{ 1 });
		const size_t requestedAlignment = NormalizeAlignment(alignment);
		const size_t allocationSize = requestedSize + requestedAlignment + sizeof(AllocationHeader);
		void* rawPointer = std::malloc(allocationSize);
		if (!rawPointer)
		{
			throw std::bad_alloc();
		}

		const uintptr_t rawAddress = reinterpret_cast<uintptr_t>(rawPointer);
		const uintptr_t alignedAddress = AlignUp(rawAddress + sizeof(AllocationHeader), requestedAlignment);
		auto* header = reinterpret_cast<AllocationHeader*>(alignedAddress - sizeof(AllocationHeader));
		header->Magic = kAllocationMagic;
		header->Tag = tag;
		header->Size = requestedSize;
		header->RawPointer = rawPointer;
		header->Previous = nullptr;
		header->Next = nullptr;

		LinkAllocation(*header);
		return reinterpret_cast<void*>(alignedAddress);
	}

	void TrackedAllocator::Free(void* pointer) noexcept
	{
		if (!pointer)
		{
			return;
		}

		auto* header = reinterpret_cast<AllocationHeader*>(reinterpret_cast<uintptr_t>(pointer) - sizeof(AllocationHeader));
		if (header->Magic != kAllocationMagic)
		{
			OutputDebugStringA("Memory: invalid or double free ignored.\n");
			return;
		}

		void* rawPointer = header->RawPointer;
		UnlinkAllocation(*header);
		header->Magic = 0;
		std::free(rawPointer);
	}

	MemorySystemStats TrackedAllocator::GetStats() const
	{
		std::scoped_lock lock(m_Mutex);
		MemorySystemStats stats;
		stats.Tags = m_TagStats;
		stats.LiveAllocationCount = m_LiveAllocationCount;
		return stats;
	}

	void TrackedAllocator::DumpLeaks() const
	{
		std::scoped_lock lock(m_Mutex);
		if (!m_Head)
		{
			OutputDebugStringA("Memory: no tracked CPU leaks.\n");
			return;
		}

		OutputDebugStringA("Memory: tracked CPU leak report begin.\n");
		for (const AllocationHeader* header = m_Head; header; header = header->Next)
		{
			const std::string message = std::format(
				"  leak tag={} size={} bytes ptr={}\n",
				ToString(header->Tag),
				header->Size,
				static_cast<const void*>(header + 1));
			OutputDebugStringA(message.c_str());
		}
		OutputDebugStringA("Memory: tracked CPU leak report end.\n");
	}

	void TrackedAllocator::LinkAllocation(AllocationHeader& header)
	{
		std::scoped_lock lock(m_Mutex);
		header.Next = m_Head;
		if (m_Head)
		{
			m_Head->Previous = &header;
		}
		m_Head = &header;

		MemoryStats& stats = m_TagStats[static_cast<size_t>(header.Tag)];
		stats.CurrentBytes += header.Size;
		stats.PeakBytes = (std::max)(stats.PeakBytes, stats.CurrentBytes);
		stats.TotalAllocatedBytes += header.Size;
		++stats.AllocationCount;
		++m_LiveAllocationCount;
	}

	void TrackedAllocator::UnlinkAllocation(AllocationHeader& header) noexcept
	{
		std::scoped_lock lock(m_Mutex);
		if (header.Previous)
		{
			header.Previous->Next = header.Next;
		}
		else if (m_Head == &header)
		{
			m_Head = header.Next;
		}

		if (header.Next)
		{
			header.Next->Previous = header.Previous;
		}

		MemoryStats& stats = m_TagStats[static_cast<size_t>(header.Tag)];
		stats.CurrentBytes = header.Size <= stats.CurrentBytes ? stats.CurrentBytes - header.Size : 0;
		++stats.FreeCount;
		if (m_LiveAllocationCount > 0)
		{
			--m_LiveAllocationCount;
		}
	}
}
