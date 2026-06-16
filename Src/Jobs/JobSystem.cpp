#include "Jobs/JobSystem.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <format>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace Jobs
{
	namespace
	{
		constexpr size_t kWorkerScratchBytes = 4ull * 1024ull * 1024ull;

		[[nodiscard]] uint32_t DefaultWorkerCount() noexcept
		{
			const uint32_t hardwareThreads = std::thread::hardware_concurrency();
			if (hardwareThreads <= 1)
			{
				return 1;
			}
			return (std::min)(hardwareThreads - 1, 8u);
		}
	}

	JobSystem::~JobSystem()
	{
		Shutdown();
	}

	void JobSystem::Initialize(uint32_t workerCount)
	{
		JobSystemConfig config;
		config.WorkerCount = workerCount;
		Initialize(config);
	}

	void JobSystem::Initialize(const JobSystemConfig& config)
	{
		if (m_Initialized.exchange(true, std::memory_order_acq_rel))
		{
			return;
		}

		m_MainThreadId = std::this_thread::get_id();
		m_AdaptiveParallelForEnabled = true;
		m_ParallelForSequentialThreshold = config.ParallelForSequentialThreshold;
		m_TargetJobsPerWorker = (std::max)(config.TargetJobsPerWorker, size_t{ 1 });
		m_SelectedBenchmarkChunkSize = 0;
		m_BenchmarkRows = {};
		m_BenchmarkRowCount = 0;

		const uint32_t actualWorkerCount = config.WorkerCount > 0 ? config.WorkerCount : DefaultWorkerCount();
		m_WorkerScratchAllocators.reserve(actualWorkerCount);
		for (uint32_t workerIndex = 0; workerIndex < actualWorkerCount; ++workerIndex)
		{
			auto scratchAllocator = std::make_unique<Memory::LinearFrameAllocator>();
			scratchAllocator->Initialize(kWorkerScratchBytes);
			m_WorkerScratchAllocators.push_back(std::move(scratchAllocator));
		}

		m_Workers.reserve(actualWorkerCount);
		for (uint32_t workerIndex = 0; workerIndex < actualWorkerCount; ++workerIndex)
		{
			m_Workers.emplace_back([this, workerIndex](std::stop_token stopToken)
				{
					WorkerLoop(stopToken, workerIndex);
				});
		}

		OutputDebugStringA(std::format("Jobs: initialized workerCount={}.\n", actualWorkerCount).c_str());
		if (config.EnableStartupBenchmark)
		{
			RunStartupBenchmark(config.BenchmarkWorkItemCount);
		}
	}

	void JobSystem::Shutdown()
	{
		if (!m_Initialized.exchange(false, std::memory_order_acq_rel))
		{
			return;
		}

		{
			std::scoped_lock lock(m_Mutex);
			for (QueuedJob& job : m_Queue)
			{
				CompleteJob(job.State);
			}
			m_Queue.clear();
		}
		m_Condition.notify_all();
		m_Workers.clear();
		for (auto& scratchAllocator : m_WorkerScratchAllocators)
		{
			if (scratchAllocator)
			{
				scratchAllocator->Shutdown();
			}
		}
		m_WorkerScratchAllocators.clear();
		m_BenchmarkRows = {};
		m_BenchmarkRowCount = 0;
		OutputDebugStringA("Jobs: shutdown.\n");
	}

	JobHandle JobSystem::Schedule(JobDesc desc)
	{
		if (!desc.Execute)
		{
			JobHandle handle(std::make_shared<JobHandle::State>());
			CompleteJob(handle.m_State);
			return handle;
		}

		if (!IsInitialized())
		{
			Initialize();
		}

		auto state = std::make_shared<JobHandle::State>();
		{
			std::scoped_lock lock(m_Mutex);
			m_Queue.push_back(QueuedJob{
				.Desc = std::move(desc),
				.State = state,
				.FrameIndex = GetFrameIndex()
				});
		}
		m_Condition.notify_one();
		return JobHandle(std::move(state));
	}

	void JobSystem::Wait(const JobHandle& handle)
	{
		if (!handle.m_State)
		{
			return;
		}

		while (true)
		{
			{
				std::unique_lock lock(handle.m_State->Mutex);
				if (handle.m_State->Completed)
				{
					return;
				}
			}

			QueuedJob job;
			bool hasLocalJob = false;
			{
				std::scoped_lock lock(m_Mutex);
				if (!m_Queue.empty())
				{
					job = std::move(m_Queue.front());
					m_Queue.pop_front();
					hasLocalJob = true;
				}
			}

			if (hasLocalJob)
			{
				ExecuteJob(job, 0, false);
				continue;
			}

			std::unique_lock lock(handle.m_State->Mutex);
			handle.m_State->Condition.wait(lock, [&handle]()
				{
					return handle.m_State->Completed;
				});
		}
	}

	void JobSystem::WaitAll(std::span<const JobHandle> handles)
	{
		for (const JobHandle& handle : handles)
		{
			Wait(handle);
		}
	}

	void JobSystem::BeginFrame() noexcept
	{
		m_FrameIndex.fetch_add(1, std::memory_order_acq_rel);
	}

	JobSystemStats JobSystem::GetStats() const
	{
		JobSystemStats stats;
		stats.WorkerCount = GetWorkerCount();
		stats.FrameIndex = GetFrameIndex();
		stats.AdaptiveParallelForEnabled = m_AdaptiveParallelForEnabled;
		stats.ParallelForSequentialThreshold = m_ParallelForSequentialThreshold;
		stats.TargetJobsPerWorker = m_TargetJobsPerWorker;
		stats.SelectedBenchmarkChunkSize = m_SelectedBenchmarkChunkSize;
		stats.BenchmarkRows = m_BenchmarkRows;
		stats.BenchmarkRowCount = m_BenchmarkRowCount;
		return stats;
	}

	size_t JobSystem::CalculateAdaptiveChunkSize(size_t count, size_t minItemsPerJob) const noexcept
	{
		const size_t sanitizedMinItemsPerJob = (std::max)(minItemsPerJob, size_t{ 1 });
		if (!m_AdaptiveParallelForEnabled || count <= m_ParallelForSequentialThreshold || GetWorkerCount() == 0)
		{
			return count;
		}

		const size_t targetJobCount = (std::max)(size_t{ 1 }, static_cast<size_t>(GetWorkerCount()) * (std::max)(m_TargetJobsPerWorker, size_t{ 1 }));
		const size_t adaptiveChunkSize = (count + targetJobCount - 1) / targetJobCount;
		return (std::max)(sanitizedMinItemsPerJob, adaptiveChunkSize);
	}

	bool JobSystem::IsWorkerThread() const noexcept
	{
		return IsInitialized() && std::this_thread::get_id() != m_MainThreadId;
	}

	std::vector<std::string> JobSystem::ConsumeErrors()
	{
		std::scoped_lock lock(m_ErrorMutex);
		std::vector<std::string> errors;
		errors.swap(m_Errors);
		return errors;
	}

	void JobSystem::WorkerLoop(std::stop_token stopToken, uint32_t workerIndex)
	{
		while (!stopToken.stop_requested())
		{
			QueuedJob job;
			{
				std::unique_lock lock(m_Mutex);
				m_Condition.wait(lock, stopToken, [this]()
					{
						return !m_Queue.empty() || !m_Initialized.load(std::memory_order_acquire);
					});

				if (stopToken.stop_requested() || !m_Initialized.load(std::memory_order_acquire))
				{
					return;
				}

				job = std::move(m_Queue.front());
				m_Queue.pop_front();
			}

			ExecuteJob(job, workerIndex, true);
		}
	}

	void JobSystem::ExecuteJob(QueuedJob& job, uint32_t workerIndex, bool isWorkerThread)
	{
		JobContext context{
			.WorkerIndex = workerIndex,
			.FrameIndex = job.FrameIndex,
			.IsWorkerThread = isWorkerThread,
			.ScratchAllocator = isWorkerThread && workerIndex < m_WorkerScratchAllocators.size()
				? m_WorkerScratchAllocators[workerIndex].get()
				: nullptr
		};

		if (context.ScratchAllocator)
		{
			context.ScratchAllocator->Reset();
		}

		try
		{
			job.Desc.Execute(context);
		}
		catch (const std::exception& exception)
		{
			PushError(std::format("Job '{}' failed: {}", job.Desc.Name ? job.Desc.Name : "Job", exception.what()));
		}
		catch (...)
		{
			PushError(std::format("Job '{}' failed with unknown exception.", job.Desc.Name ? job.Desc.Name : "Job"));
		}

		CompleteJob(job.State);
	}

	void JobSystem::CompleteJob(const std::shared_ptr<JobHandle::State>& state) noexcept
	{
		if (!state)
		{
			return;
		}

		{
			std::scoped_lock lock(state->Mutex);
			state->Completed = true;
		}
		state->Condition.notify_all();
	}

	void JobSystem::PushError(std::string message)
	{
		OutputDebugStringA((message + "\n").c_str());
		std::scoped_lock lock(m_ErrorMutex);
		m_Errors.push_back(std::move(message));
	}

	void JobSystem::RunStartupBenchmark(size_t workItemCount)
	{
		if (workItemCount == 0)
		{
			return;
		}

		const size_t count = (std::max)(workItemCount, size_t{ 10000 });
		std::vector<float> input(count);
		std::vector<float> output(count);
		for (size_t index = 0; index < count; ++index)
		{
			input[index] = static_cast<float>((index % 1024) + 1) * 0.001f;
		}

		const auto runKernel = [&input, &output](size_t begin, size_t end)
			{
				for (size_t index = begin; index < end; ++index)
				{
					float value = input[index];
					for (uint32_t iteration = 0; iteration < 24; ++iteration)
					{
						value = value * 1.000123f + 0.00031f;
						value -= static_cast<float>(iteration & 3u) * 0.00007f;
					}
					output[index] = value;
				}
			};

		const auto measureMilliseconds = [](auto&& action)
			{
				const auto begin = std::chrono::steady_clock::now();
				action();
				const auto end = std::chrono::steady_clock::now();
				return std::chrono::duration<double, std::milli>(end - begin).count();
			};

		const auto pushRow = [this, count](const char* name, size_t chunkSize, double milliseconds, double sequentialMilliseconds)
			{
				if (m_BenchmarkRowCount >= m_BenchmarkRows.size())
				{
					return;
				}

				m_BenchmarkRows[m_BenchmarkRowCount++] = JobBenchmarkRow{
					.Name = name,
					.WorkItemCount = count,
					.ChunkSize = chunkSize,
					.WorkerCount = GetWorkerCount(),
					.Milliseconds = milliseconds,
					.SpeedupVsSequential = milliseconds > 0.0 ? sequentialMilliseconds / milliseconds : 0.0
				};
			};

		const double sequentialMilliseconds = measureMilliseconds([&]()
			{
				runKernel(0, count);
			});
		pushRow("No Job sequential", count, sequentialMilliseconds, sequentialMilliseconds);

		struct Candidate
		{
			const char* Name = "";
			size_t ChunkSize = 1;
		};

		const std::array candidates = {
			Candidate{ "Job chunk 1", 1 },
			Candidate{ "Job chunk 64", 64 },
			Candidate{ "Job chunk 256", 256 },
			Candidate{ "Job chunk 1024", 1024 },
			Candidate{ "Job chunk 4096", 4096 }
		};

		double bestJobMilliseconds = (std::numeric_limits<double>::max)();
		size_t bestChunkSize = count;
		for (const Candidate& candidate : candidates)
		{
			const double milliseconds = measureMilliseconds([&]()
				{
					std::vector<JobHandle> handles = ParallelFor(
						count,
						candidate.ChunkSize,
						candidate.Name,
						[&runKernel](size_t begin, size_t end, JobContext&)
						{
							runKernel(begin, end);
						});
					WaitAll(handles);
				});
			pushRow(candidate.Name, candidate.ChunkSize, milliseconds, sequentialMilliseconds);
			if (milliseconds < bestJobMilliseconds)
			{
				bestJobMilliseconds = milliseconds;
				bestChunkSize = candidate.ChunkSize;
			}
		}

		const size_t previousTargetJobsPerWorker = m_TargetJobsPerWorker;
		const size_t bestChunkCount = (count + bestChunkSize - 1) / bestChunkSize;
		if (GetWorkerCount() > 0 && bestChunkCount > 0)
		{
			m_TargetJobsPerWorker = (std::clamp)(bestChunkCount / (std::max)(static_cast<size_t>(GetWorkerCount()), size_t{ 1 }), size_t{ 1 }, size_t{ 16 });
		}
		m_SelectedBenchmarkChunkSize = bestChunkSize;

		const double adaptiveMilliseconds = measureMilliseconds([&]()
			{
				RunParallelFor(
					count,
					1,
					"Job adaptive",
					[&runKernel](size_t begin, size_t end, JobContext&)
					{
						runKernel(begin, end);
					});
			});
		pushRow("Job adaptive selected", CalculateAdaptiveChunkSize(count, 1), adaptiveMilliseconds, sequentialMilliseconds);

		OutputDebugStringA("Jobs benchmark results:\n");
		for (size_t rowIndex = 0; rowIndex < m_BenchmarkRowCount; ++rowIndex)
		{
			const JobBenchmarkRow& row = m_BenchmarkRows[rowIndex];
			OutputDebugStringA(std::format(
				"  {}: {:.4f} ms speedup={:.2f}x chunk={} workers={}\n",
				row.Name,
				row.Milliseconds,
				row.SpeedupVsSequential,
				row.ChunkSize,
				row.WorkerCount).c_str());
		}
		OutputDebugStringA(std::format(
			"Jobs policy: adaptive targetJobsPerWorker={} selectedBenchmarkChunk={} previousTargetJobsPerWorker={}\n",
			m_TargetJobsPerWorker,
			m_SelectedBenchmarkChunkSize,
			previousTargetJobsPerWorker).c_str());
	}
}
