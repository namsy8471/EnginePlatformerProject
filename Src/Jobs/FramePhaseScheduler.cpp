#include "Jobs/FramePhaseScheduler.h"

#include <utility>

namespace Jobs
{
	FramePhaseScheduler::FramePhaseScheduler(JobSystem& jobSystem) noexcept
		: m_JobSystem(&jobSystem)
	{
	}

	void FramePhaseScheduler::SetJobSystem(JobSystem& jobSystem) noexcept
	{
		m_JobSystem = &jobSystem;
	}

	void FramePhaseScheduler::BeginFrame(uint64_t frameIndex) noexcept
	{
		m_FrameIndex = frameIndex;
		m_CurrentPhase = FramePhase::BeginFrame;
		m_CurrentPhaseJobs.clear();
	}

	void FramePhaseScheduler::BeginPhase(FramePhase phase)
	{
		WaitForCurrentPhase();
		m_CurrentPhase = phase;
		m_CurrentPhaseJobs.clear();
	}

	JobHandle FramePhaseScheduler::Schedule(JobDesc desc)
	{
		if (!m_JobSystem)
		{
			return {};
		}

		JobHandle handle = m_JobSystem->Schedule(std::move(desc));
		m_CurrentPhaseJobs.push_back(handle);
		return handle;
	}

	void FramePhaseScheduler::WaitForCurrentPhase()
	{
		if (m_JobSystem && !m_CurrentPhaseJobs.empty())
		{
			m_JobSystem->WaitAll(m_CurrentPhaseJobs);
		}
		m_CurrentPhaseJobs.clear();
	}

	void FramePhaseScheduler::EndPhase()
	{
		WaitForCurrentPhase();
	}

	void FramePhaseScheduler::EndFrame()
	{
		WaitForCurrentPhase();
		m_CurrentPhase = FramePhase::EndFrame;
	}
}
