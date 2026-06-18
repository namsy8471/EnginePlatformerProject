#include "Scripting/ScriptRuntime.h"

#include "Math/MathHelpers.h"

#include <DirectXMath.h>

#include <algorithm>
#include <format>
#include <utility>

namespace Scripting
{
	namespace
	{
		[[nodiscard]] constexpr const char* PhaseName(ScriptPhase phase) noexcept
		{
			switch (phase)
			{
			case ScriptPhase::Start:
				return "Start";
			case ScriptPhase::Update:
				return "Update";
			case ScriptPhase::LateUpdate:
				return "LateUpdate";
			case ScriptPhase::EndFrame:
				return "EndFrame";
			default:
				return "Unknown";
			}
		}

		void EnqueueSpinCommand(ScriptRuntimeContext& context, float radiansPerSecond)
		{
			if (!context.ActiveScene || !context.SceneCommands)
			{
				return;
			}

			Scene* scene = context.ActiveScene;
			const EntityId entityId = context.Entity;
			const float deltaAngle = radiansPerSecond * context.DeltaTime;
			context.SceneCommands->Enqueue([scene, entityId, deltaAngle]()
				{
					TransformComponent* transform = scene->GetTransformComponent(entityId);
					if (!transform)
					{
						return;
					}

					const DirectX::XMVECTOR deltaRotation = DirectX::XMQuaternionRotationRollPitchYaw(0.0f, deltaAngle, 0.0f);
					const DirectX::XMVECTOR currentRotation = DirectX::XMLoadFloat4(&transform->LocalTransform.Rotation);
					DirectX::XMStoreFloat4(
						&transform->LocalTransform.Rotation,
						DirectX::XMQuaternionNormalize(DirectX::XMQuaternionMultiply(deltaRotation, currentRotation)));
					transform->LocalTransform.Rotation = Math::NormalizeQuaternionOrIdentity(transform->LocalTransform.Rotation);
					transform->UpdateWorld();
				});
		}
	}

	void NativeScriptRuntime::RegisterDefaultScripts()
	{
		m_Definitions.clear();

		RegisterNativeScript(NativeScriptDefinition{
			.ClassName = "GameScript",
			.ScheduleUpdateOnWorker = true,
			.OnStart = [this](ScriptRuntimeContext& context)
			{
				PushLog(std::format("Script Start - Entity={} Class=GameScript", context.Entity));
			}
		});

		RegisterNativeScript(NativeScriptDefinition{
			.ClassName = "SpinScript",
			.ScheduleUpdateOnWorker = true,
			.OnStart = [this](ScriptRuntimeContext& context)
			{
				PushLog(std::format("Script Start - Entity={} Class=SpinScript", context.Entity));
			},
			.OnUpdate = [](ScriptRuntimeContext& context)
			{
				EnqueueSpinCommand(context, 1.25f);
			}
		});
	}

	void NativeScriptRuntime::RegisterNativeScript(NativeScriptDefinition definition)
	{
		if (definition.ClassName.empty())
		{
			return;
		}

		m_Definitions[definition.ClassName] = std::move(definition);
		m_Stats.RegisteredNativeScriptCount = m_Definitions.size();
	}

	void NativeScriptRuntime::Reset()
	{
		m_StartedEntities.clear();
		m_Stats.ActiveScriptCount = 0;
		m_Stats.StartedScriptCount = 0;
		m_Stats.ScheduledJobCount = 0;
		m_Stats.MissingNativeScriptCount = 0;
	}

	void NativeScriptRuntime::ClearEntity(EntityId entityId)
	{
		m_StartedEntities.erase(entityId);
	}

	void NativeScriptRuntime::RunStart(Scene& scene, Jobs::SceneCommandBuffer& sceneCommands, float deltaTime, uint64_t frameIndex, bool isPlaying)
	{
		m_Stats.ActiveScriptCount = 0;
		m_Stats.ScheduledJobCount = 0;
		m_Stats.MissingNativeScriptCount = 0;

		for (const SceneEntity& entity : scene.GetEntities())
		{
			const ScriptComponent* script = scene.GetScriptComponent(entity.Id);
			const bool enabled = scene.IsComponentEnabled<ScriptComponent>(entity.Id);
			if (!script || !ShouldRunScript(*script, enabled, isPlaying))
			{
				ClearEntity(entity.Id);
				continue;
			}

			++m_Stats.ActiveScriptCount;
			const NativeScriptDefinition* definition = FindDefinition(script->ClassName);
			if (!definition)
			{
				++m_Stats.MissingNativeScriptCount;
				continue;
			}

			const auto startedIt = m_StartedEntities.find(entity.Id);
			if (startedIt != m_StartedEntities.end() && startedIt->second == script->ClassName)
			{
				continue;
			}

			m_StartedEntities[entity.Id] = script->ClassName;
			ScriptRuntimeContext context{
				.ActiveScene = &scene,
				.SceneCommands = &sceneCommands,
				.Entity = entity.Id,
				.Script = script,
				.DeltaTime = deltaTime,
				.FrameIndex = frameIndex,
				.IsPlaying = isPlaying,
				.Phase = ScriptPhase::Start
			};
			if (definition->OnStart)
			{
				definition->OnStart(context);
			}
		}

		m_Stats.StartedScriptCount = m_StartedEntities.size();
		m_Stats.RegisteredNativeScriptCount = m_Definitions.size();
	}

	void NativeScriptRuntime::ScheduleUpdate(Scene& scene, Jobs::FramePhaseScheduler& scheduler, Jobs::SceneCommandBuffer& sceneCommands, float deltaTime, uint64_t frameIndex, bool isPlaying)
	{
		SchedulePhase(scene, scheduler, sceneCommands, deltaTime, frameIndex, isPlaying, ScriptPhase::Update);
	}

	void NativeScriptRuntime::ScheduleLateUpdate(Scene& scene, Jobs::FramePhaseScheduler& scheduler, Jobs::SceneCommandBuffer& sceneCommands, float deltaTime, uint64_t frameIndex, bool isPlaying)
	{
		SchedulePhase(scene, scheduler, sceneCommands, deltaTime, frameIndex, isPlaying, ScriptPhase::LateUpdate);
	}

	void NativeScriptRuntime::ScheduleEndFrame(Scene& scene, Jobs::FramePhaseScheduler& scheduler, Jobs::SceneCommandBuffer& sceneCommands, float deltaTime, uint64_t frameIndex, bool isPlaying)
	{
		SchedulePhase(scene, scheduler, sceneCommands, deltaTime, frameIndex, isPlaying, ScriptPhase::EndFrame);
	}

	std::vector<std::string> NativeScriptRuntime::ConsumeLogs()
	{
		std::vector<std::string> logs;
		logs.swap(m_Logs);
		return logs;
	}

	void NativeScriptRuntime::SchedulePhase(
		Scene& scene,
		Jobs::FramePhaseScheduler& scheduler,
		Jobs::SceneCommandBuffer& sceneCommands,
		float deltaTime,
		uint64_t frameIndex,
		bool isPlaying,
		ScriptPhase phase)
	{
		for (const SceneEntity& entity : scene.GetEntities())
		{
			const ScriptComponent* script = scene.GetScriptComponent(entity.Id);
			const bool enabled = scene.IsComponentEnabled<ScriptComponent>(entity.Id);
			if (!script || !ShouldRunScript(*script, enabled, isPlaying))
			{
				continue;
			}

			const auto startedIt = m_StartedEntities.find(entity.Id);
			if (startedIt == m_StartedEntities.end() || startedIt->second != script->ClassName)
			{
				continue;
			}

			const NativeScriptDefinition* definition = FindDefinition(script->ClassName);
			if (!definition)
			{
				continue;
			}

			NativeScriptCallback callback;
			switch (phase)
			{
			case ScriptPhase::Update:
				callback = definition->OnUpdate;
				break;
			case ScriptPhase::LateUpdate:
				callback = definition->OnLateUpdate;
				break;
			case ScriptPhase::EndFrame:
				callback = definition->OnEndFrame;
				break;
			default:
				break;
			}
			if (!callback)
			{
				continue;
			}

			ScriptRuntimeContext context{
				.ActiveScene = &scene,
				.SceneCommands = &sceneCommands,
				.Entity = entity.Id,
				.Script = script,
				.DeltaTime = deltaTime,
				.FrameIndex = frameIndex,
				.IsPlaying = isPlaying,
				.Phase = phase
			};

			if (definition->ScheduleUpdateOnWorker)
			{
				static_cast<void>(scheduler.Schedule(Jobs::JobDesc{
					.Name = "NativeScript",
					.Execute = [callback = std::move(callback), context](Jobs::JobContext&) mutable
					{
						callback(context);
					}
				}));
				++m_Stats.ScheduledJobCount;
			}
			else
			{
				callback(context);
			}
		}

		m_Stats.StartedScriptCount = m_StartedEntities.size();
		m_Stats.RegisteredNativeScriptCount = m_Definitions.size();
		(void)PhaseName(phase);
	}

	const NativeScriptDefinition* NativeScriptRuntime::FindDefinition(std::string_view className) const
	{
		const auto it = m_Definitions.find(std::string(className));
		return it != m_Definitions.end() ? &it->second : nullptr;
	}

	bool NativeScriptRuntime::ShouldRunScript(const ScriptComponent& script, bool componentEnabled, bool isPlaying) noexcept
	{
		return componentEnabled &&
			script.Language == ScriptLanguage::Native &&
			!script.ClassName.empty() &&
			(isPlaying || script.RunInEditor);
	}

	void NativeScriptRuntime::PushLog(std::string message)
	{
		if (!message.empty())
		{
			m_Logs.push_back(std::move(message));
		}
	}
}
