#pragma once

#include "Jobs/FramePhaseScheduler.h"
#include "Jobs/SceneCommandBuffer.h"

#include <cstdint>

namespace Jobs
{
	struct SystemContext
	{
		JobSystem* Jobs = nullptr;
		SceneCommandBuffer* SceneCommands = nullptr;
		float DeltaTime = 0.0f;
		uint64_t FrameIndex = 0;
	};

	class IEngineSystem
	{
	public:
		virtual ~IEngineSystem() = default;

		virtual void OnStart(SystemContext&) {}
		virtual void ScheduleFixedUpdate(SystemContext&, FramePhaseScheduler&) {}
		virtual void ScheduleUpdate(SystemContext&, FramePhaseScheduler&) {}
		virtual void ScheduleLateUpdate(SystemContext&, FramePhaseScheduler&) {}
		virtual void ScheduleAnimation(SystemContext&, FramePhaseScheduler&) {}
		virtual void SchedulePhysics(SystemContext&, FramePhaseScheduler&) {}
		virtual void ScheduleRenderPrepare(SystemContext&, FramePhaseScheduler&) {}
		virtual void ScheduleEndFrame(SystemContext&, FramePhaseScheduler&) {}
	};
}
