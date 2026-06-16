#pragma once

#include "Jobs/JobSystem.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Jobs
{
	enum class FramePhase : uint8_t
	{
		BeginFrame,
		DrainMainThreadQueues,
		Start,
		FixedUpdate,
		Update,
		LateUpdate,
		Animation,
		Physics,
		RenderPrepare,
		Commit,
		EndFrame,
		Count
	};

	[[nodiscard]] constexpr std::string_view ToString(FramePhase phase) noexcept
	{
		switch (phase)
		{
		case FramePhase::BeginFrame:
			return "BeginFrame";
		case FramePhase::DrainMainThreadQueues:
			return "DrainMainThreadQueues";
		case FramePhase::Start:
			return "Start";
		case FramePhase::FixedUpdate:
			return "FixedUpdate";
		case FramePhase::Update:
			return "Update";
		case FramePhase::LateUpdate:
			return "LateUpdate";
		case FramePhase::Animation:
			return "Animation";
		case FramePhase::Physics:
			return "Physics";
		case FramePhase::RenderPrepare:
			return "RenderPrepare";
		case FramePhase::Commit:
			return "Commit";
		case FramePhase::EndFrame:
			return "EndFrame";
		default:
			return "Unknown";
		}
	}

	inline constexpr std::array kOrderedFramePhases = {
		FramePhase::BeginFrame,
		FramePhase::DrainMainThreadQueues,
		FramePhase::Start,
		FramePhase::FixedUpdate,
		FramePhase::Update,
		FramePhase::LateUpdate,
		FramePhase::Animation,
		FramePhase::Physics,
		FramePhase::RenderPrepare,
		FramePhase::Commit,
		FramePhase::EndFrame
	};

	class FramePhaseScheduler
	{
	public:
		FramePhaseScheduler() = default;
		explicit FramePhaseScheduler(JobSystem& jobSystem) noexcept;

		void SetJobSystem(JobSystem& jobSystem) noexcept;
		void BeginFrame(uint64_t frameIndex) noexcept;
		void BeginPhase(FramePhase phase);
		[[nodiscard]] JobHandle Schedule(JobDesc desc);
		void WaitForCurrentPhase();
		void EndPhase();
		void EndFrame();

		template <typename Function>
		void RunPhase(FramePhase phase, Function&& function)
		{
			BeginPhase(phase);
			function(*this);
			EndPhase();
		}

		[[nodiscard]] FramePhase GetCurrentPhase() const noexcept { return m_CurrentPhase; }
		[[nodiscard]] uint64_t GetFrameIndex() const noexcept { return m_FrameIndex; }

	private:
		JobSystem* m_JobSystem = nullptr;
		FramePhase m_CurrentPhase = FramePhase::BeginFrame;
		uint64_t m_FrameIndex = 0;
		std::vector<JobHandle> m_CurrentPhaseJobs;
	};
}
