#pragma once

#include "Memory/LinearFrameAllocator.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Jobs
{
	struct JobContext
	{
		uint32_t WorkerIndex = 0;
		uint64_t FrameIndex = 0;
		bool IsWorkerThread = false;
		Memory::LinearFrameAllocator* ScratchAllocator = nullptr;
	};

	struct JobDesc
	{
		const char* Name = "Job";
		std::function<void(JobContext&)> Execute;
	};

	inline constexpr size_t kJobBenchmarkMaxRows = 12;

	struct JobBenchmarkRow
	{
		const char* Name = "";
		size_t WorkItemCount = 0;
		size_t ChunkSize = 0;
		uint32_t WorkerCount = 0;
		double Milliseconds = 0.0;
		double SpeedupVsSequential = 0.0;
	};

	struct JobSystemStats
	{
		uint32_t WorkerCount = 0;
		uint64_t FrameIndex = 0;
		bool AdaptiveParallelForEnabled = true;
		size_t ParallelForSequentialThreshold = 0;
		size_t TargetJobsPerWorker = 0;
		size_t SelectedBenchmarkChunkSize = 0;
		std::array<JobBenchmarkRow, kJobBenchmarkMaxRows> BenchmarkRows = {};
		size_t BenchmarkRowCount = 0;
	};

	struct JobSystemConfig
	{
		uint32_t WorkerCount = 0;
		bool EnableStartupBenchmark = true;
		size_t ParallelForSequentialThreshold = 2048;
		size_t TargetJobsPerWorker = 4;
		size_t BenchmarkWorkItemCount = 200000;
	};

	class JobHandle
	{
	public:
		JobHandle() = default;
		[[nodiscard]] bool IsValid() const noexcept { return static_cast<bool>(m_State); }

	private:
		struct State
		{
			std::mutex Mutex;
			std::condition_variable Condition;
			bool Completed = false;
		};

		explicit JobHandle(std::shared_ptr<State> state) noexcept
			: m_State(std::move(state))
		{
		}

		std::shared_ptr<State> m_State;

		friend class JobSystem;
	};

	class JobSystem
	{
	public:
		JobSystem() = default;
		~JobSystem();

		JobSystem(const JobSystem&) = delete;
		JobSystem& operator=(const JobSystem&) = delete;

		void Initialize(uint32_t workerCount = 0);
		void Initialize(const JobSystemConfig& config);
		void Shutdown();

		[[nodiscard]] JobHandle Schedule(JobDesc desc);
		void Wait(const JobHandle& handle);
		void WaitAll(std::span<const JobHandle> handles);

		template <typename Function>
		[[nodiscard]] std::vector<JobHandle> ParallelFor(size_t count, size_t chunkSize, const char* name, Function&& function)
		{
			std::vector<JobHandle> handles;
			if (count == 0)
			{
				return handles;
			}

			const size_t sanitizedChunkSize = chunkSize > 0 ? chunkSize : 1;
			const size_t chunkCount = (count + sanitizedChunkSize - 1) / sanitizedChunkSize;
			handles.reserve(chunkCount);
			auto sharedFunction = std::make_shared<std::decay_t<Function>>(std::forward<Function>(function));
			for (size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
			{
				const size_t begin = chunkIndex * sanitizedChunkSize;
				const size_t end = (std::min)(begin + sanitizedChunkSize, count);
				handles.push_back(Schedule(JobDesc{
					.Name = name,
					.Execute = [begin, end, sharedFunction](JobContext& context)
					{
						(*sharedFunction)(begin, end, context);
					}
					}));
			}
			return handles;
		}

		template <typename Function>
		void RunParallelFor(size_t count, size_t minItemsPerJob, const char* name, Function&& function)
		{
			if (count == 0)
			{
				return;
			}

			if (!IsInitialized())
			{
				Initialize();
			}

			const size_t chunkSize = CalculateAdaptiveChunkSize(count, minItemsPerJob);
			const size_t chunkCount = (count + chunkSize - 1) / chunkSize;
			if (chunkCount <= 1)
			{
				JobContext context{
					.WorkerIndex = 0,
					.FrameIndex = GetFrameIndex(),
					.IsWorkerThread = false,
					.ScratchAllocator = nullptr
				};
				function(0, count, context);
				return;
			}

			std::vector<JobHandle> handles = ParallelFor(count, chunkSize, name, std::forward<Function>(function));
			WaitAll(handles);
		}

		void BeginFrame() noexcept;
		[[nodiscard]] uint64_t GetFrameIndex() const noexcept { return m_FrameIndex.load(std::memory_order_acquire); }
		[[nodiscard]] uint32_t GetWorkerCount() const noexcept { return static_cast<uint32_t>(m_Workers.size()); }
		[[nodiscard]] JobSystemStats GetStats() const;
		[[nodiscard]] size_t CalculateAdaptiveChunkSize(size_t count, size_t minItemsPerJob) const noexcept;
		[[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized.load(std::memory_order_acquire); }
		[[nodiscard]] bool IsWorkerThread() const noexcept;
		[[nodiscard]] std::vector<std::string> ConsumeErrors();

	private:
		struct QueuedJob
		{
			JobDesc Desc;
			std::shared_ptr<JobHandle::State> State;
			uint64_t FrameIndex = 0;
		};

		void WorkerLoop(std::stop_token stopToken, uint32_t workerIndex);
		void ExecuteJob(QueuedJob& job, uint32_t workerIndex, bool isWorkerThread);
		void CompleteJob(const std::shared_ptr<JobHandle::State>& state) noexcept;
		void PushError(std::string message);
		void RunStartupBenchmark(size_t workItemCount);

		mutable std::mutex m_Mutex;
		std::condition_variable_any m_Condition;
		std::deque<QueuedJob> m_Queue;
		std::vector<std::jthread> m_Workers;
		std::vector<std::unique_ptr<Memory::LinearFrameAllocator>> m_WorkerScratchAllocators;
		std::thread::id m_MainThreadId;
		std::atomic_uint64_t m_FrameIndex = 0;
		std::atomic_bool m_Initialized = false;
		bool m_AdaptiveParallelForEnabled = true;
		size_t m_ParallelForSequentialThreshold = 2048;
		size_t m_TargetJobsPerWorker = 4;
		size_t m_SelectedBenchmarkChunkSize = 0;
		std::array<JobBenchmarkRow, kJobBenchmarkMaxRows> m_BenchmarkRows = {};
		size_t m_BenchmarkRowCount = 0;

		mutable std::mutex m_ErrorMutex;
		std::vector<std::string> m_Errors;
	};

	template <typename Function>
	[[nodiscard]] std::vector<JobHandle> ParallelFor(JobSystem& jobSystem, size_t count, size_t chunkSize, const char* name, Function&& function)
	{
		return jobSystem.ParallelFor(count, chunkSize, name, std::forward<Function>(function));
	}
}
