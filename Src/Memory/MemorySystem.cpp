#include "Memory/MemorySystem.h"

#include "Memory/FixedBlockPoolAllocator.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace Memory
{
	namespace
	{
		constexpr uint32_t kPooledAllocationMagic = 0x4D504F4Cu;
		constexpr std::array<size_t, 5> kSmallPoolPayloadSizes = { 32, 64, 128, 256, 512 };
		constexpr size_t kSmallPoolCount = kSmallPoolPayloadSizes.size();

		struct PooledAllocationHeader
		{
			uint32_t Magic = 0;
			uint16_t PoolIndex = 0;
			uint16_t Reserved = 0;
			uint32_t RequestedSize = 0;
			MemoryTag Tag = MemoryTag::General;
		};

		[[nodiscard]] constexpr size_t AlignUp(size_t value, size_t alignment) noexcept
		{
			return (value + alignment - 1) & ~(alignment - 1);
		}

		constexpr size_t kPooledHeaderSize = AlignUp(sizeof(PooledAllocationHeader), alignof(std::max_align_t));

		struct MemorySystemState
		{
			TrackedAllocator Tracked;
			LinearFrameAllocator Frame;
			std::array<FixedBlockPoolAllocator, kSmallPoolCount> SmallPools;
			std::array<MemoryStats, kMemoryTagCount> PooledTagStats = {};
			std::array<MemoryBenchmarkRow, kMemoryBenchmarkMaxRows> BenchmarkRows = {};
			MemoryConfig Config;
			std::thread::id MainThreadId;
			size_t BenchmarkRowCount = 0;
			bool SmallPoolRoutingEnabled = false;
			bool Initialized = false;
			std::mutex Mutex;
		};

		MemorySystemState& State()
		{
			static MemorySystemState state;
			return state;
		}

		volatile uint8_t g_BenchmarkSink = 0;

		void TouchMemory(void* pointer, size_t size, uint32_t salt) noexcept
		{
			if (!pointer || size == 0)
			{
				return;
			}

			auto* bytes = static_cast<std::byte*>(pointer);
			bytes[0] = static_cast<std::byte>(salt & 0xffu);
			bytes[size - 1] = static_cast<std::byte>((salt >> 8u) & 0xffu);
			const uint8_t value = std::to_integer<uint8_t>(bytes[0]) ^ std::to_integer<uint8_t>(bytes[size - 1]);
			g_BenchmarkSink = static_cast<uint8_t>(g_BenchmarkSink ^ value);
		}

		[[nodiscard]] size_t FindSmallPoolIndex(size_t requestedSize) noexcept
		{
			for (size_t poolIndex = 0; poolIndex < kSmallPoolPayloadSizes.size(); ++poolIndex)
			{
				if (requestedSize <= kSmallPoolPayloadSizes[poolIndex])
				{
					return poolIndex;
				}
			}

			return kSmallPoolCount;
		}

		void AddAllocationStats(MemoryStats& stats, size_t size) noexcept
		{
			stats.CurrentBytes += size;
			stats.PeakBytes = (std::max)(stats.PeakBytes, stats.CurrentBytes);
			stats.TotalAllocatedBytes += size;
			++stats.AllocationCount;
		}

		void RemoveAllocationStats(MemoryStats& stats, size_t size) noexcept
		{
			stats.CurrentBytes = size <= stats.CurrentBytes ? stats.CurrentBytes - size : 0;
			++stats.FreeCount;
		}

		[[nodiscard]] double MeasureNanosecondsPerOperation(size_t operationCount, auto&& action)
		{
			const auto start = std::chrono::steady_clock::now();
			action();
			const auto end = std::chrono::steady_clock::now();
			const auto elapsed = std::chrono::duration<double, std::nano>(end - start).count();
			return elapsed / static_cast<double>((std::max)(operationCount, size_t{ 1 }));
		}

		[[nodiscard]] double BenchmarkCrtFixed(size_t blockSize, size_t iterations)
		{
			std::vector<void*> pointers;
			pointers.reserve(iterations);
			return MeasureNanosecondsPerOperation(iterations * 2, [&]()
				{
					for (size_t index = 0; index < iterations; ++index)
					{
						void* pointer = std::malloc(blockSize);
						if (!pointer)
						{
							throw std::bad_alloc();
						}
						TouchMemory(pointer, blockSize, static_cast<uint32_t>(index));
						pointers.push_back(pointer);
					}
					for (void* pointer : pointers)
					{
						std::free(pointer);
					}
				});
		}

		[[nodiscard]] double BenchmarkTrackedFixed(size_t blockSize, size_t iterations)
		{
			TrackedAllocator allocator;
			std::vector<void*> pointers;
			pointers.reserve(iterations);
			return MeasureNanosecondsPerOperation(iterations * 2, [&]()
				{
					for (size_t index = 0; index < iterations; ++index)
					{
						void* pointer = allocator.Allocate(blockSize, alignof(std::max_align_t), MemoryTag::General);
						TouchMemory(pointer, blockSize, static_cast<uint32_t>(index));
						pointers.push_back(pointer);
					}
					for (void* pointer : pointers)
					{
						allocator.Free(pointer);
					}
				});
		}

		[[nodiscard]] double BenchmarkFixedPoolFixed(size_t blockSize, size_t iterations, size_t blocksPerPage)
		{
			FixedBlockPoolAllocator allocator;
			allocator.Initialize(blockSize, alignof(std::max_align_t), blocksPerPage, MemoryTag::General);
			std::vector<void*> pointers;
			pointers.reserve(iterations);
			const double result = MeasureNanosecondsPerOperation(iterations * 2, [&]()
				{
					for (size_t index = 0; index < iterations; ++index)
					{
						void* pointer = allocator.Allocate();
						TouchMemory(pointer, blockSize, static_cast<uint32_t>(index));
						pointers.push_back(pointer);
					}
					for (void* pointer : pointers)
					{
						allocator.Free(pointer);
					}
				});
			allocator.Shutdown();
			return result;
		}

		[[nodiscard]] double BenchmarkLinearFrameFixed(size_t blockSize, size_t iterations)
		{
			LinearFrameAllocator allocator;
			allocator.Initialize(AlignUp(blockSize, alignof(std::max_align_t)) * iterations + alignof(std::max_align_t));
			const double result = MeasureNanosecondsPerOperation(iterations, [&]()
				{
					for (size_t index = 0; index < iterations; ++index)
					{
						void* pointer = allocator.Allocate(blockSize, alignof(std::max_align_t));
						TouchMemory(pointer, blockSize, static_cast<uint32_t>(index));
					}
					allocator.Reset();
				});
			allocator.Shutdown();
			return result;
		}

		[[nodiscard]] size_t MixedSizeFor(size_t index) noexcept
		{
			constexpr std::array<size_t, 8> pattern = { 24, 40, 64, 96, 128, 192, 320, 448 };
			return pattern[index % pattern.size()];
		}

		[[nodiscard]] double BenchmarkCrtMixed(size_t iterations)
		{
			std::vector<std::pair<void*, size_t>> pointers;
			pointers.reserve(iterations);
			return MeasureNanosecondsPerOperation(iterations * 2, [&]()
				{
					for (size_t index = 0; index < iterations; ++index)
					{
						const size_t blockSize = MixedSizeFor(index);
						void* pointer = std::malloc(blockSize);
						if (!pointer)
						{
							throw std::bad_alloc();
						}
						TouchMemory(pointer, blockSize, static_cast<uint32_t>(index));
						pointers.emplace_back(pointer, blockSize);
					}
					for (const auto& [pointer, blockSize] : pointers)
					{
						(void)blockSize;
						std::free(pointer);
					}
				});
		}

		[[nodiscard]] double BenchmarkTrackedMixed(size_t iterations)
		{
			TrackedAllocator allocator;
			std::vector<std::pair<void*, size_t>> pointers;
			pointers.reserve(iterations);
			return MeasureNanosecondsPerOperation(iterations * 2, [&]()
				{
					for (size_t index = 0; index < iterations; ++index)
					{
						const size_t blockSize = MixedSizeFor(index);
						void* pointer = allocator.Allocate(blockSize, alignof(std::max_align_t), MemoryTag::General);
						TouchMemory(pointer, blockSize, static_cast<uint32_t>(index));
						pointers.emplace_back(pointer, blockSize);
					}
					for (const auto& [pointer, blockSize] : pointers)
					{
						(void)blockSize;
						allocator.Free(pointer);
					}
				});
		}

		[[nodiscard]] double BenchmarkFixedPoolMixed(size_t iterations, size_t blocksPerPage)
		{
			std::array<FixedBlockPoolAllocator, kSmallPoolCount> allocators;
			for (size_t poolIndex = 0; poolIndex < allocators.size(); ++poolIndex)
			{
				allocators[poolIndex].Initialize(kSmallPoolPayloadSizes[poolIndex], alignof(std::max_align_t), blocksPerPage, MemoryTag::General);
			}

			std::vector<std::pair<void*, size_t>> pointers;
			pointers.reserve(iterations);
			const double result = MeasureNanosecondsPerOperation(iterations * 2, [&]()
				{
					for (size_t index = 0; index < iterations; ++index)
					{
						const size_t blockSize = MixedSizeFor(index);
						const size_t poolIndex = FindSmallPoolIndex(blockSize);
						void* pointer = allocators[poolIndex].Allocate();
						TouchMemory(pointer, blockSize, static_cast<uint32_t>(index));
						pointers.emplace_back(pointer, poolIndex);
					}
					for (const auto& [pointer, poolIndex] : pointers)
					{
						allocators[poolIndex].Free(pointer);
					}
				});

			for (FixedBlockPoolAllocator& allocator : allocators)
			{
				allocator.Shutdown();
			}
			return result;
		}

		[[nodiscard]] double BenchmarkRoutedFixedPoolMixed(size_t iterations, size_t blocksPerPage)
		{
			std::array<FixedBlockPoolAllocator, kSmallPoolCount> allocators;
			for (size_t poolIndex = 0; poolIndex < allocators.size(); ++poolIndex)
			{
				allocators[poolIndex].Initialize(
					kPooledHeaderSize + kSmallPoolPayloadSizes[poolIndex],
					alignof(std::max_align_t),
					blocksPerPage,
					MemoryTag::General);
			}

			std::vector<void*> pointers;
			pointers.reserve(iterations);
			const double result = MeasureNanosecondsPerOperation(iterations * 2, [&]()
				{
					for (size_t index = 0; index < iterations; ++index)
					{
						const size_t blockSize = MixedSizeFor(index);
						const size_t poolIndex = FindSmallPoolIndex(blockSize);
						auto* block = static_cast<std::byte*>(allocators[poolIndex].Allocate());
						auto* header = reinterpret_cast<PooledAllocationHeader*>(block);
						header->Magic = kPooledAllocationMagic;
						header->PoolIndex = static_cast<uint16_t>(poolIndex);
						header->Reserved = 0;
						header->RequestedSize = static_cast<uint32_t>(blockSize);
						header->Tag = MemoryTag::General;

						void* payload = block + kPooledHeaderSize;
						TouchMemory(payload, blockSize, static_cast<uint32_t>(index));
						pointers.push_back(payload);
					}

					for (void* pointer : pointers)
					{
						auto* block = static_cast<std::byte*>(pointer) - kPooledHeaderSize;
						auto* header = reinterpret_cast<PooledAllocationHeader*>(block);
						if (header->Magic == kPooledAllocationMagic && header->PoolIndex < allocators.size())
						{
							const size_t owningPoolIndex = header->PoolIndex;
							header->Magic = 0;
							allocators[owningPoolIndex].Free(header);
						}
					}
				});

			for (FixedBlockPoolAllocator& allocator : allocators)
			{
				allocator.Shutdown();
			}
			return result;
		}

		void PushBenchmarkRow(MemorySystemState& state, std::string_view name, size_t blockSize, size_t iterations, double nsPerOperation, double baseline)
		{
			if (state.BenchmarkRowCount >= state.BenchmarkRows.size())
			{
				return;
			}

			state.BenchmarkRows[state.BenchmarkRowCount++] = MemoryBenchmarkRow{
				.Name = name,
				.BlockSize = blockSize,
				.Iterations = iterations,
				.NanosecondsPerOperation = nsPerOperation,
				.SpeedupVsBaseline = baseline > 0.0 ? baseline / nsPerOperation : 0.0
			};
		}

		void RunStartupAllocatorBenchmarks(MemorySystemState& state)
		{
			state.BenchmarkRowCount = 0;
			state.BenchmarkRows = {};

			const size_t iterations = (std::max)(state.Config.StartupBenchmarkIterations, size_t{ 1000 });
			constexpr size_t fixedBlockSize = 64;

			const double crtFixed = BenchmarkCrtFixed(fixedBlockSize, iterations);
			const double trackedFixed = BenchmarkTrackedFixed(fixedBlockSize, iterations);
			const double fixedPoolFixed = BenchmarkFixedPoolFixed(fixedBlockSize, iterations, state.Config.SmallBlockPoolBlocksPerPage);
			const double linearFixed = BenchmarkLinearFrameFixed(fixedBlockSize, iterations);
			const double crtMixed = BenchmarkCrtMixed(iterations);
			const double trackedMixed = BenchmarkTrackedMixed(iterations);
			const double fixedPoolMixed = BenchmarkFixedPoolMixed(iterations, state.Config.SmallBlockPoolBlocksPerPage);
			const double routedFixedPoolMixed = BenchmarkRoutedFixedPoolMixed(iterations, state.Config.SmallBlockPoolBlocksPerPage);

			PushBenchmarkRow(state, "CRT 64B alloc/free", fixedBlockSize, iterations, crtFixed, crtFixed);
			PushBenchmarkRow(state, "Tracked 64B alloc/free", fixedBlockSize, iterations, trackedFixed, crtFixed);
			PushBenchmarkRow(state, "FixedPool 64B alloc/free", fixedBlockSize, iterations, fixedPoolFixed, crtFixed);
			PushBenchmarkRow(state, "LinearFrame 64B alloc/reset", fixedBlockSize, iterations, linearFixed, crtFixed);
			PushBenchmarkRow(state, "CRT mixed small", kSmallPoolPayloadSizes.back(), iterations, crtMixed, crtMixed);
			PushBenchmarkRow(state, "Tracked mixed small", kSmallPoolPayloadSizes.back(), iterations, trackedMixed, crtMixed);
			PushBenchmarkRow(state, "FixedPool mixed small", kSmallPoolPayloadSizes.back(), iterations, fixedPoolMixed, crtMixed);
			PushBenchmarkRow(state, "Routed FixedPool mixed", kSmallPoolPayloadSizes.back(), iterations, routedFixedPoolMixed, crtMixed);

			state.SmallPoolRoutingEnabled = state.Config.EnableSmallBlockPools
				&& routedFixedPoolMixed <= crtMixed
				&& routedFixedPoolMixed <= trackedMixed;

			OutputDebugStringA("Memory benchmark results:\n");
			for (size_t rowIndex = 0; rowIndex < state.BenchmarkRowCount; ++rowIndex)
			{
				const MemoryBenchmarkRow& row = state.BenchmarkRows[rowIndex];
				OutputDebugStringA(std::format(
					"  {}: {:.2f} ns/op ({:.2f}x baseline)\n",
					row.Name,
					row.NanosecondsPerOperation,
					row.SpeedupVsBaseline).c_str());
			}

			OutputDebugStringA(std::format(
				"Memory policy: frame=LinearFrame, small-main-thread={}<=512B, fallback=Tracked.\n",
				state.SmallPoolRoutingEnabled ? "FixedBlockPool" : "Tracked").c_str());
		}

		void InitializeSmallPools(MemorySystemState& state)
		{
			for (size_t poolIndex = 0; poolIndex < state.SmallPools.size(); ++poolIndex)
			{
				const size_t payloadSize = kSmallPoolPayloadSizes[poolIndex];
				const size_t blockSize = kPooledHeaderSize + payloadSize;
				state.SmallPools[poolIndex].Initialize(
					blockSize,
					alignof(std::max_align_t),
					state.Config.SmallBlockPoolBlocksPerPage,
					MemoryTag::General);
			}
		}

		void ShutdownSmallPools(MemorySystemState& state) noexcept
		{
			for (FixedBlockPoolAllocator& pool : state.SmallPools)
			{
				pool.Shutdown();
			}
			state.PooledTagStats = {};
		}

		[[nodiscard]] void* AllocateSmallBlock(MemorySystemState& state, size_t size, MemoryTag tag)
		{
			const size_t poolIndex = FindSmallPoolIndex(size);
			if (poolIndex >= state.SmallPools.size())
			{
				return nullptr;
			}

			auto* block = static_cast<std::byte*>(state.SmallPools[poolIndex].Allocate());
			auto* header = reinterpret_cast<PooledAllocationHeader*>(block);
			header->Magic = kPooledAllocationMagic;
			header->PoolIndex = static_cast<uint16_t>(poolIndex);
			header->Reserved = 0;
			header->RequestedSize = static_cast<uint32_t>(size);
			header->Tag = tag;

			AddAllocationStats(state.PooledTagStats[static_cast<size_t>(tag)], size);
			return block + kPooledHeaderSize;
		}

		[[nodiscard]] bool TryFreeSmallBlock(MemorySystemState& state, void* pointer) noexcept
		{
			auto* block = static_cast<std::byte*>(pointer) - kPooledHeaderSize;
			auto* header = reinterpret_cast<PooledAllocationHeader*>(block);
			if (header->Magic == kPooledAllocationMagic)
			{
				const size_t poolIndex = header->PoolIndex;
				if (poolIndex >= state.SmallPools.size())
				{
					OutputDebugStringA("Memory: pooled allocation header owner mismatch ignored.\n");
					return true;
				}

				RemoveAllocationStats(state.PooledTagStats[static_cast<size_t>(header->Tag)], header->RequestedSize);
				header->Magic = 0;
				state.SmallPools[poolIndex].Free(header);
				return true;
			}

			for (size_t poolIndex = 0; poolIndex < state.SmallPools.size(); ++poolIndex)
			{
				if (state.SmallPools[poolIndex].Contains(block))
				{
					OutputDebugStringA("Memory: invalid or double pooled free ignored.\n");
					return true;
				}
			}

			return false;
		}

		void DumpSmallPoolLeaks(const MemorySystemState& state)
		{
			size_t liveBytes = 0;
			size_t liveAllocations = 0;
			for (size_t tagIndex = 0; tagIndex < state.PooledTagStats.size(); ++tagIndex)
			{
				const MemoryStats& stats = state.PooledTagStats[tagIndex];
				if (stats.CurrentBytes == 0)
				{
					continue;
				}

				const size_t liveCount = stats.AllocationCount - stats.FreeCount;
				liveBytes += stats.CurrentBytes;
				liveAllocations += liveCount;
				OutputDebugStringA(std::format(
					"Memory: pooled leak tag={} allocations={} bytes={}\n",
					ToString(static_cast<MemoryTag>(tagIndex)),
					liveCount,
					stats.CurrentBytes).c_str());
			}

			if (liveAllocations > 0)
			{
				OutputDebugStringA(std::format(
					"Memory: pooled CPU leak summary allocations={} bytes={}\n",
					liveAllocations,
					liveBytes).c_str());
			}
		}
	}

	void Initialize(const MemoryConfig& config)
	{
		MemorySystemState& state = State();
		std::scoped_lock lock(state.Mutex);
		if (state.Initialized)
		{
			return;
		}

		state.Config = config;
		state.MainThreadId = std::this_thread::get_id();
		state.Frame.Initialize(config.FrameArenaSizeBytes);
		InitializeSmallPools(state);
		state.SmallPoolRoutingEnabled = config.EnableSmallBlockPools;
		if (config.EnableStartupBenchmark)
		{
			RunStartupAllocatorBenchmarks(state);
		}
		state.Initialized = true;
		OutputDebugStringA(std::format("Memory: initialized frame arena={} bytes.\n", config.FrameArenaSizeBytes).c_str());
	}

	void Shutdown()
	{
		MemorySystemState& state = State();
		std::scoped_lock lock(state.Mutex);
		if (!state.Initialized)
		{
			return;
		}

		state.Tracked.DumpLeaks();
		DumpSmallPoolLeaks(state);
		ShutdownSmallPools(state);
		state.Frame.Shutdown();
		state.BenchmarkRows = {};
		state.BenchmarkRowCount = 0;
		state.SmallPoolRoutingEnabled = false;
		state.Initialized = false;
		OutputDebugStringA("Memory: shutdown.\n");
	}

	void BeginFrame()
	{
		MemorySystemState& state = State();
		if (!state.Initialized)
		{
			return;
		}
		state.Frame.Reset();
	}

	void EndFrame()
	{
	}

	void* Allocate(size_t size, size_t alignment, MemoryTag tag)
	{
		MemorySystemState& state = State();
		if (!state.Initialized)
		{
			Initialize();
		}

		if (tag == MemoryTag::Frame)
		{
			return state.Frame.Allocate(size, alignment);
		}

		const size_t requestedSize = (std::max)(size, size_t{ 1 });
		if (state.SmallPoolRoutingEnabled
			&& std::this_thread::get_id() == state.MainThreadId
			&& alignment <= alignof(std::max_align_t)
			&& requestedSize <= kSmallPoolPayloadSizes.back())
		{
			if (void* pooledPointer = AllocateSmallBlock(state, requestedSize, tag))
			{
				return pooledPointer;
			}
		}

		return state.Tracked.Allocate(size, alignment, tag);
	}

	void Free(void* pointer) noexcept
	{
		if (!pointer)
		{
			return;
		}

		MemorySystemState& state = State();
		if (state.Initialized && state.Frame.Contains(pointer))
		{
			return;
		}

		if (state.Initialized && TryFreeSmallBlock(state, pointer))
		{
			return;
		}

		state.Tracked.Free(pointer);
	}

	MemorySystemStats GetStats()
	{
		MemorySystemState& state = State();
		MemorySystemStats stats = state.Tracked.GetStats();
		for (size_t tagIndex = 0; tagIndex < kMemoryTagCount; ++tagIndex)
		{
			const MemoryStats& pooledStats = state.PooledTagStats[tagIndex];
			MemoryStats& tagStats = stats.Tags[tagIndex];
			tagStats.CurrentBytes += pooledStats.CurrentBytes;
			tagStats.PeakBytes = (std::max)(tagStats.PeakBytes, pooledStats.PeakBytes);
			tagStats.TotalAllocatedBytes += pooledStats.TotalAllocatedBytes;
			tagStats.AllocationCount += pooledStats.AllocationCount;
			tagStats.FreeCount += pooledStats.FreeCount;
			stats.LiveAllocationCount += pooledStats.AllocationCount - pooledStats.FreeCount;
		}

		const MemoryStats frameStats = state.Frame.GetStats();
		stats.Tags[static_cast<size_t>(MemoryTag::Frame)] = frameStats;
		stats.FrameArenaCapacity = state.Frame.GetCapacity();
		stats.FrameArenaCurrent = state.Frame.GetCurrentOffset();
		stats.FrameArenaPeak = state.Frame.GetPeakOffset();
		stats.SmallPoolRoutingEnabled = state.SmallPoolRoutingEnabled;
		stats.SmallPoolMaxBlockSize = kSmallPoolPayloadSizes.back();
		stats.BenchmarkRows = state.BenchmarkRows;
		stats.BenchmarkRowCount = state.BenchmarkRowCount;
		for (const FixedBlockPoolAllocator& pool : state.SmallPools)
		{
			const MemoryStats poolStats = pool.GetStats();
			stats.SmallPoolCurrentBytes += poolStats.CurrentBytes;
			stats.SmallPoolPeakBytes += poolStats.PeakBytes;
			stats.SmallPoolAllocationCount += poolStats.AllocationCount;
			stats.SmallPoolFreeCount += poolStats.FreeCount;
		}
		return stats;
	}

	void DumpLeaks()
	{
		const MemorySystemState& state = State();
		state.Tracked.DumpLeaks();
		DumpSmallPoolLeaks(state);
	}

	IAllocator& GetAllocator(MemoryTag)
	{
		return State().Tracked;
	}

	std::span<const MemoryStats, kMemoryTagCount> GetTagStats()
	{
		static std::array<MemoryStats, kMemoryTagCount> statsSnapshot = {};
		statsSnapshot = GetStats().Tags;
		return statsSnapshot;
	}
}
