#pragma once

#include "Memory/IAllocator.h"

#include <array>
#include <cstddef>
#include <mutex>

namespace Memory
{
	class TrackedAllocator final : public IAllocator
	{
	public:
		TrackedAllocator() = default;
		~TrackedAllocator() override = default;

		TrackedAllocator(const TrackedAllocator&) = delete;
		TrackedAllocator& operator=(const TrackedAllocator&) = delete;

		[[nodiscard]] void* Allocate(size_t size, size_t alignment, MemoryTag tag) override;
		void Free(void* pointer) noexcept override;
		[[nodiscard]] MemorySystemStats GetStats() const override;
		void DumpLeaks() const;

	private:
		struct AllocationHeader
		{
			uint32_t Magic = 0;
			MemoryTag Tag = MemoryTag::General;
			size_t Size = 0;
			void* RawPointer = nullptr;
			AllocationHeader* Previous = nullptr;
			AllocationHeader* Next = nullptr;
		};

		static constexpr uint32_t kAllocationMagic = 0x454D454Du;

		void LinkAllocation(AllocationHeader& header);
		void UnlinkAllocation(AllocationHeader& header) noexcept;

		mutable std::mutex m_Mutex;
		std::array<MemoryStats, kMemoryTagCount> m_TagStats = {};
		AllocationHeader* m_Head = nullptr;
		size_t m_LiveAllocationCount = 0;
	};
}
