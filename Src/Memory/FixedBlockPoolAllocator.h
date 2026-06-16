#pragma once

#include "Memory/MemoryTypes.h"

#include <cstddef>
#include <vector>

namespace Memory
{
	class FixedBlockPoolAllocator
	{
	public:
		FixedBlockPoolAllocator() = default;
		~FixedBlockPoolAllocator();

		FixedBlockPoolAllocator(const FixedBlockPoolAllocator&) = delete;
		FixedBlockPoolAllocator& operator=(const FixedBlockPoolAllocator&) = delete;

		void Initialize(size_t blockSize, size_t blockAlignment, size_t blocksPerPage, MemoryTag tag);
		void Shutdown() noexcept;
		[[nodiscard]] void* Allocate();
		void Free(void* pointer) noexcept;
		[[nodiscard]] bool Contains(const void* pointer) const noexcept;
		[[nodiscard]] MemoryStats GetStats() const noexcept { return m_Stats; }
		[[nodiscard]] size_t GetBlockSize() const noexcept { return m_BlockSize; }
		[[nodiscard]] size_t GetReservedBytes() const noexcept { return m_BlockSize * m_BlocksPerPage * m_Pages.size(); }

	private:
		struct FreeNode
		{
			FreeNode* Next = nullptr;
		};

		void AllocatePage();

		size_t m_BlockSize = 0;
		size_t m_BlockAlignment = 0;
		size_t m_BlocksPerPage = 0;
		MemoryTag m_Tag = MemoryTag::General;
		FreeNode* m_FreeList = nullptr;
		std::vector<void*> m_Pages;
		MemoryStats m_Stats = {};
	};
}
