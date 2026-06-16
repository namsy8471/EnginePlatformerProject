#include "Memory/FixedBlockPoolAllocator.h"

#include <algorithm>
#include <malloc.h>
#include <new>

namespace Memory
{
	namespace
	{
		[[nodiscard]] size_t AlignUp(size_t value, size_t alignment) noexcept
		{
			const size_t mask = alignment - 1;
			return (value + mask) & ~mask;
		}
	}

	FixedBlockPoolAllocator::~FixedBlockPoolAllocator()
	{
		Shutdown();
	}

	void FixedBlockPoolAllocator::Initialize(size_t blockSize, size_t blockAlignment, size_t blocksPerPage, MemoryTag tag)
	{
		Shutdown();
		m_BlockAlignment = (std::max)(blockAlignment, alignof(std::max_align_t));
		m_BlockSize = AlignUp((std::max)(blockSize, sizeof(FreeNode)), m_BlockAlignment);
		m_BlocksPerPage = (std::max)(blocksPerPage, size_t{ 1 });
		m_Tag = tag;
	}

	void FixedBlockPoolAllocator::Shutdown() noexcept
	{
		for (void* page : m_Pages)
		{
			_aligned_free(page);
		}
		m_Pages.clear();
		m_FreeList = nullptr;
		m_BlockSize = 0;
		m_BlockAlignment = 0;
		m_BlocksPerPage = 0;
		m_Stats = {};
	}

	void* FixedBlockPoolAllocator::Allocate()
	{
		if (!m_FreeList)
		{
			AllocatePage();
		}

		FreeNode* node = m_FreeList;
		m_FreeList = m_FreeList->Next;
		m_Stats.CurrentBytes += m_BlockSize;
		m_Stats.PeakBytes = (std::max)(m_Stats.PeakBytes, m_Stats.CurrentBytes);
		m_Stats.TotalAllocatedBytes += m_BlockSize;
		++m_Stats.AllocationCount;
		return node;
	}

	void FixedBlockPoolAllocator::Free(void* pointer) noexcept
	{
		if (!pointer)
		{
			return;
		}

		auto* node = static_cast<FreeNode*>(pointer);
		node->Next = m_FreeList;
		m_FreeList = node;
		m_Stats.CurrentBytes = m_BlockSize <= m_Stats.CurrentBytes ? m_Stats.CurrentBytes - m_BlockSize : 0;
		++m_Stats.FreeCount;
	}

	bool FixedBlockPoolAllocator::Contains(const void* pointer) const noexcept
	{
		if (!pointer || m_BlockSize == 0 || m_BlocksPerPage == 0)
		{
			return false;
		}

		const auto* target = static_cast<const std::byte*>(pointer);
		const size_t pageSize = m_BlockSize * m_BlocksPerPage;
		for (const void* page : m_Pages)
		{
			const auto* pageBegin = static_cast<const std::byte*>(page);
			const auto* pageEnd = pageBegin + pageSize;
			if (target >= pageBegin && target < pageEnd)
			{
				return true;
			}
		}

		return false;
	}

	void FixedBlockPoolAllocator::AllocatePage()
	{
		if (m_BlockSize == 0 || m_BlocksPerPage == 0)
		{
			throw std::bad_alloc();
		}

		const size_t pageSize = m_BlockSize * m_BlocksPerPage;
		void* page = _aligned_malloc(pageSize, m_BlockAlignment);
		if (!page)
		{
			throw std::bad_alloc();
		}
		m_Pages.push_back(page);

		auto* pageBytes = static_cast<std::byte*>(page);
		for (size_t blockIndex = 0; blockIndex < m_BlocksPerPage; ++blockIndex)
		{
			auto* node = reinterpret_cast<FreeNode*>(pageBytes + blockIndex * m_BlockSize);
			node->Next = m_FreeList;
			m_FreeList = node;
		}
	}
}
