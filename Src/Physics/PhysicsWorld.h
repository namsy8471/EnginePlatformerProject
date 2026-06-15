#pragma once

#include "Physics/PhysXBackend.h"
#include "Scene/SceneTypes.h"

#include <DirectXMath.h>

#include <unordered_map>

class Scene;

namespace physx
{
	class PxRigidActor;
	class PxMaterial;
}

namespace Physics
{
	class PhysicsWorld
	{
	public:
		[[nodiscard]] bool Initialize();
		void Shutdown();
		void Clear();
		void CreateOrUpdateActor(EntityId entityId, Scene& scene);
		void RemoveActor(EntityId entityId);
		void Step(Scene& scene, float deltaTime);
		void SetGravity(DirectX::XMFLOAT3 gravity);

		[[nodiscard]] bool IsInitialized() const noexcept;
		[[nodiscard]] bool HasActor(EntityId entityId) const;

	private:
		struct ActorEntry
		{
			physx::PxRigidActor* Actor = nullptr;
			physx::PxMaterial* Material = nullptr;
		};

		void SyncKinematicActors(Scene& scene);
		void SyncDynamicActors(Scene& scene);
		[[nodiscard]] ActorEntry CreateActorEntry(EntityId entityId, Scene& scene);

		PhysXBackend m_Backend;
		std::unordered_map<EntityId, ActorEntry> m_Actors;
		DirectX::XMFLOAT3 m_Gravity = { 0.0f, -9.81f, 0.0f };
		float m_FixedTimeStep = 1.0f / 60.0f;
		float m_Accumulator = 0.0f;
		uint32_t m_MaxSubsteps = 4;
	};
}
