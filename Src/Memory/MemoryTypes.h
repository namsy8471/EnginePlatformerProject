#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Memory
{
	enum class MemoryTag : uint8_t
	{
		General,
		Frame,
		Scene,
		ECS,
		Asset,
		Animation,
		Rendering,
		Physics,
		Editor,
		Count
	};

	inline constexpr size_t kMemoryTagCount = static_cast<size_t>(MemoryTag::Count);
	inline constexpr size_t kMemoryBenchmarkMaxRows = 8;

	struct MemoryStats
	{
		size_t CurrentBytes = 0;
		size_t PeakBytes = 0;
		size_t TotalAllocatedBytes = 0;
		size_t AllocationCount = 0;
		size_t FreeCount = 0;
	};

	struct MemoryBenchmarkRow
	{
		std::string_view Name = {};
		size_t BlockSize = 0;
		size_t Iterations = 0;
		double NanosecondsPerOperation = 0.0;
		double SpeedupVsBaseline = 0.0;
	};

	struct MemorySystemStats
	{
		std::array<MemoryStats, kMemoryTagCount> Tags = {};
		size_t LiveAllocationCount = 0;
		size_t FrameArenaCapacity = 0;
		size_t FrameArenaCurrent = 0;
		size_t FrameArenaPeak = 0;
		bool SmallPoolRoutingEnabled = false;
		size_t SmallPoolMaxBlockSize = 0;
		size_t SmallPoolCurrentBytes = 0;
		size_t SmallPoolPeakBytes = 0;
		size_t SmallPoolAllocationCount = 0;
		size_t SmallPoolFreeCount = 0;
		std::array<MemoryBenchmarkRow, kMemoryBenchmarkMaxRows> BenchmarkRows = {};
		size_t BenchmarkRowCount = 0;
	};

	struct MemoryConfig
	{
		size_t FrameArenaSizeBytes = 64ull * 1024ull * 1024ull;
		bool EnableStartupBenchmark = true;
		bool EnableSmallBlockPools = true;
		size_t SmallBlockPoolBlocksPerPage = 4096;
		size_t StartupBenchmarkIterations = 20000;
	};

	[[nodiscard]] constexpr std::string_view ToString(MemoryTag tag) noexcept
	{
		switch (tag)
		{
		case MemoryTag::General:
			return "General";
		case MemoryTag::Frame:
			return "Frame";
		case MemoryTag::Scene:
			return "Scene";
		case MemoryTag::ECS:
			return "ECS";
		case MemoryTag::Asset:
			return "Asset";
		case MemoryTag::Animation:
			return "Animation";
		case MemoryTag::Rendering:
			return "Rendering";
		case MemoryTag::Physics:
			return "Physics";
		case MemoryTag::Editor:
			return "Editor";
		default:
			return "Unknown";
		}
	}
}
