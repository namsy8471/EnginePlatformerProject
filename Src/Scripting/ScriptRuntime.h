#pragma once

#include "Jobs/FramePhaseScheduler.h"
#include "Jobs/SceneCommandBuffer.h"
#include "Scene/Scene.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Scripting
{
	enum class ScriptPhase : uint8_t
	{
		Start,
		Update,
		LateUpdate,
		EndFrame
	};

	struct ScriptRuntimeContext
	{
		Scene* ActiveScene = nullptr;
		Jobs::SceneCommandBuffer* SceneCommands = nullptr;
		EntityId Entity = InvalidEntityId;
		const ScriptComponent* Script = nullptr;
		float DeltaTime = 0.0f;
		uint64_t FrameIndex = 0;
		bool IsPlaying = false;
		ScriptPhase Phase = ScriptPhase::Update;
	};

	using NativeScriptCallback = std::function<void(ScriptRuntimeContext&)>;

	struct NativeScriptDefinition
	{
		std::string ClassName;
		bool ScheduleUpdateOnWorker = true;
		NativeScriptCallback OnStart;
		NativeScriptCallback OnUpdate;
		NativeScriptCallback OnLateUpdate;
		NativeScriptCallback OnEndFrame;
	};

	struct ScriptRuntimeStats
	{
		size_t RegisteredNativeScriptCount = 0;
		size_t ActiveScriptCount = 0;
		size_t StartedScriptCount = 0;
		size_t ScheduledJobCount = 0;
		size_t MissingNativeScriptCount = 0;
	};

	class NativeScriptRuntime
	{
	public:
		void RegisterDefaultScripts();
		void RegisterNativeScript(NativeScriptDefinition definition);
		void Reset();
		void ClearEntity(EntityId entityId);

		void RunStart(Scene& scene, Jobs::SceneCommandBuffer& sceneCommands, float deltaTime, uint64_t frameIndex, bool isPlaying);
		void ScheduleUpdate(Scene& scene, Jobs::FramePhaseScheduler& scheduler, Jobs::SceneCommandBuffer& sceneCommands, float deltaTime, uint64_t frameIndex, bool isPlaying);
		void ScheduleLateUpdate(Scene& scene, Jobs::FramePhaseScheduler& scheduler, Jobs::SceneCommandBuffer& sceneCommands, float deltaTime, uint64_t frameIndex, bool isPlaying);
		void ScheduleEndFrame(Scene& scene, Jobs::FramePhaseScheduler& scheduler, Jobs::SceneCommandBuffer& sceneCommands, float deltaTime, uint64_t frameIndex, bool isPlaying);

		[[nodiscard]] ScriptRuntimeStats GetStats() const noexcept { return m_Stats; }
		[[nodiscard]] std::vector<std::string> ConsumeLogs();

	private:
		void SchedulePhase(
			Scene& scene,
			Jobs::FramePhaseScheduler& scheduler,
			Jobs::SceneCommandBuffer& sceneCommands,
			float deltaTime,
			uint64_t frameIndex,
			bool isPlaying,
			ScriptPhase phase);

		[[nodiscard]] const NativeScriptDefinition* FindDefinition(std::string_view className) const;
		[[nodiscard]] static bool ShouldRunScript(const ScriptComponent& script, bool componentEnabled, bool isPlaying) noexcept;
		void PushLog(std::string message);

		std::unordered_map<std::string, NativeScriptDefinition> m_Definitions;
		std::unordered_map<EntityId, std::string> m_StartedEntities;
		std::vector<std::string> m_Logs;
		ScriptRuntimeStats m_Stats = {};
	};
}
