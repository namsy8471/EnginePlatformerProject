#include "Physics/PhysicsWorld.h"

#include "Physics/PhysicsComponents.h"
#include "Scene/Scene.h"

#include <PxPhysicsAPI.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Physics
{
	namespace
	{
		[[nodiscard]] physx::PxVec3 ToPxVec3(const DirectX::XMFLOAT3& value) noexcept
		{
			return physx::PxVec3(value.x, value.y, value.z);
		}

		[[nodiscard]] DirectX::XMFLOAT3 ToFloat3(const physx::PxVec3& value) noexcept
		{
			return DirectX::XMFLOAT3(value.x, value.y, value.z);
		}

		[[nodiscard]] physx::PxQuat ToPxQuat(const DirectX::XMFLOAT4& value) noexcept
		{
			physx::PxQuat result(value.x, value.y, value.z, value.w);
			if (!result.isFinite() || result.magnitudeSquared() <= std::numeric_limits<float>::epsilon())
			{
				return physx::PxQuat(physx::PxIdentity);
			}
			result.normalize();
			return result;
		}

		[[nodiscard]] DirectX::XMFLOAT4 ToFloat4(const physx::PxQuat& value) noexcept
		{
			return DirectX::XMFLOAT4(value.x, value.y, value.z, value.w);
		}

		[[nodiscard]] float Positive(float value, float fallback) noexcept
		{
			return std::isfinite(value) && value > 0.001f ? value : fallback;
		}

		[[nodiscard]] DirectX::XMFLOAT3 AbsScale(const DirectX::XMFLOAT3& scale) noexcept
		{
			return DirectX::XMFLOAT3(
				(std::max)(std::abs(scale.x), 0.001f),
				(std::max)(std::abs(scale.y), 0.001f),
				(std::max)(std::abs(scale.z), 0.001f));
		}

		[[nodiscard]] physx::PxTransform ToPxTransform(const Math::Transform& transform) noexcept
		{
			return physx::PxTransform(ToPxVec3(transform.Translation), ToPxQuat(transform.Rotation));
		}

		[[nodiscard]] physx::PxTransform ToPlaneTransform(const Math::Transform& transform, const ColliderComponent& collider) noexcept
		{
			const physx::PxVec3 position = ToPxVec3(DirectX::XMFLOAT3(
				transform.Translation.x + collider.Offset.x,
				transform.Translation.y + collider.Offset.y,
				transform.Translation.z + collider.Offset.z));
			return physx::PxTransform(position, ToPxQuat(transform.Rotation));
		}

		[[nodiscard]] physx::PxMaterial* CreateMaterial(
			physx::PxPhysics& physics,
			const PhysicsMaterialComponent* materialComponent,
			physx::PxMaterial& fallback)
		{
			if (!materialComponent)
			{
				fallback.acquireReference();
				return &fallback;
			}

			const float staticFriction = (std::max)(materialComponent->StaticFriction, 0.0f);
			const float dynamicFriction = (std::max)(materialComponent->DynamicFriction, 0.0f);
			const float restitution = std::clamp(materialComponent->Restitution, 0.0f, 1.0f);
			return physics.createMaterial(staticFriction, dynamicFriction, restitution);
		}

		[[nodiscard]] physx::PxGeometryHolder CreateGeometry(
			const ColliderComponent& collider,
			const Math::Transform& transform,
			physx::PxTransform& shapeLocalPose)
		{
			const DirectX::XMFLOAT3 scale = AbsScale(transform.Scale);
			shapeLocalPose = physx::PxTransform(ToPxVec3(collider.Offset));
			switch (collider.Shape)
			{
			case ColliderShape::Sphere:
			{
				const float maxScale = (std::max)(scale.x, (std::max)(scale.y, scale.z));
				return physx::PxSphereGeometry(Positive(collider.Radius * maxScale, 0.5f));
			}
			case ColliderShape::Capsule:
			{
				const float radiusScale = (std::max)(scale.x, scale.z);
				const float radius = Positive(collider.Radius * radiusScale, 0.25f);
				const float scaledHeight = Positive(collider.Height * scale.y, radius * 2.0f);
				const float halfCylinderHeight = (std::max)((scaledHeight * 0.5f) - radius, 0.001f);
				shapeLocalPose.q = physx::PxQuat(physx::PxPiDivTwo, physx::PxVec3(0.0f, 0.0f, 1.0f));
				return physx::PxCapsuleGeometry(radius, halfCylinderHeight);
			}
			case ColliderShape::Plane:
				return physx::PxPlaneGeometry();
			case ColliderShape::Box:
			default:
				return physx::PxBoxGeometry(
					Positive(collider.Size.x * scale.x * 0.5f, 0.5f),
					Positive(collider.Size.y * scale.y * 0.5f, 0.5f),
					Positive(collider.Size.z * scale.z * 0.5f, 0.5f));
			}
		}
	}

	bool PhysicsWorld::Initialize()
	{
		return m_Backend.Initialize(m_Gravity);
	}

	void PhysicsWorld::Shutdown()
	{
		Clear();
		m_Backend.Shutdown();
	}

	void PhysicsWorld::Clear()
	{
		for (auto& [entityId, entry] : m_Actors)
		{
			(void)entityId;
			if (entry.Actor)
			{
				entry.Actor->release();
				entry.Actor = nullptr;
			}
			if (entry.Material)
			{
				entry.Material->release();
				entry.Material = nullptr;
			}
		}
		m_Actors.clear();
		m_Accumulator = 0.0f;
	}

	void PhysicsWorld::CreateOrUpdateActor(EntityId entityId, Scene& scene)
	{
		RemoveActor(entityId);
		if (!m_Backend.IsInitialized())
		{
			return;
		}

		ActorEntry entry = CreateActorEntry(entityId, scene);
		if (!entry.Actor)
		{
			if (entry.Material)
			{
				entry.Material->release();
			}
			return;
		}

		m_Backend.GetScene()->addActor(*entry.Actor);
		m_Actors[entityId] = entry;
	}

	void PhysicsWorld::RemoveActor(EntityId entityId)
	{
		const auto actorIt = m_Actors.find(entityId);
		if (actorIt == m_Actors.end())
		{
			return;
		}

		if (actorIt->second.Actor)
		{
			actorIt->second.Actor->release();
		}
		if (actorIt->second.Material)
		{
			actorIt->second.Material->release();
		}
		m_Actors.erase(actorIt);
	}

	void PhysicsWorld::Step(Scene& scene, float deltaTime)
	{
		if (!m_Backend.IsInitialized() || deltaTime <= 0.0f)
		{
			return;
		}

		const float maxAccumulatedTime = m_FixedTimeStep * static_cast<float>(m_MaxSubsteps);
		m_Accumulator = (std::min)(m_Accumulator + deltaTime, maxAccumulatedTime);

		uint32_t substepCount = 0;
		while (m_Accumulator >= m_FixedTimeStep && substepCount < m_MaxSubsteps)
		{
			SyncKinematicActors(scene);
			m_Backend.GetScene()->simulate(m_FixedTimeStep);
			m_Backend.GetScene()->fetchResults(true);
			m_Accumulator -= m_FixedTimeStep;
			++substepCount;
		}

		SyncDynamicActors(scene);
	}

	void PhysicsWorld::SetGravity(DirectX::XMFLOAT3 gravity)
	{
		m_Gravity = gravity;
		m_Backend.SetGravity(m_Gravity);
	}

	bool PhysicsWorld::IsInitialized() const noexcept
	{
		return m_Backend.IsInitialized();
	}

	bool PhysicsWorld::HasActor(EntityId entityId) const
	{
		return m_Actors.contains(entityId);
	}

	void PhysicsWorld::SyncKinematicActors(Scene& scene)
	{
		for (const auto& [entityId, entry] : m_Actors)
		{
			const RigidBodyComponent* rigidBody = scene.GetRigidBodyComponent(entityId);
			const TransformComponent* transform = scene.GetTransformComponent(entityId);
			if (!rigidBody || !scene.IsRigidBodyEnabled(entityId) || !transform || rigidBody->Type != RigidBodyType::Kinematic)
			{
				continue;
			}

			if (auto* dynamicActor = entry.Actor ? entry.Actor->is<physx::PxRigidDynamic>() : nullptr)
			{
				dynamicActor->setKinematicTarget(ToPxTransform(transform->WorldTransform));
			}
		}
	}

	void PhysicsWorld::SyncDynamicActors(Scene& scene)
	{
		for (const auto& [entityId, entry] : m_Actors)
		{
			RigidBodyComponent* rigidBody = scene.GetRigidBodyComponent(entityId);
			TransformComponent* transform = scene.GetTransformComponent(entityId);
			if (!rigidBody || !scene.IsRigidBodyEnabled(entityId) || !transform || rigidBody->Type != RigidBodyType::Dynamic || !entry.Actor)
			{
				continue;
			}

			physx::PxRigidDynamic* dynamicActor = entry.Actor->is<physx::PxRigidDynamic>();
			if (!dynamicActor || dynamicActor->getRigidBodyFlags().isSet(physx::PxRigidBodyFlag::eKINEMATIC))
			{
				continue;
			}

			const physx::PxTransform pose = dynamicActor->getGlobalPose();
			transform->LocalTransform.Translation = ToFloat3(pose.p);
			transform->LocalTransform.Rotation = ToFloat4(pose.q);
			transform->UpdateWorld();
			rigidBody->LinearVelocity = ToFloat3(dynamicActor->getLinearVelocity());
			rigidBody->AngularVelocity = ToFloat3(dynamicActor->getAngularVelocity());
		}
	}

	PhysicsWorld::ActorEntry PhysicsWorld::CreateActorEntry(EntityId entityId, Scene& scene)
	{
		ActorEntry entry;
		if (!scene.ContainsEntity(entityId))
		{
			return entry;
		}

		const ColliderComponent* collider = scene.GetColliderComponent(entityId);
		const TransformComponent* transform = scene.GetTransformComponent(entityId);
		if (!collider || !scene.IsColliderEnabled(entityId) || !transform)
		{
			return entry;
		}

		physx::PxPhysics* physics = m_Backend.GetPhysics();
		physx::PxMaterial* defaultMaterial = m_Backend.GetDefaultMaterial();
		if (!physics || !defaultMaterial)
		{
			return entry;
		}

		const PhysicsMaterialComponent* materialComponent = scene.IsPhysicsMaterialEnabled(entityId)
			? scene.GetPhysicsMaterialComponent(entityId)
			: nullptr;
		entry.Material = CreateMaterial(*physics, materialComponent, *defaultMaterial);
		if (!entry.Material)
		{
			return entry;
		}

		const RigidBodyComponent* rigidBody = scene.IsRigidBodyEnabled(entityId)
			? scene.GetRigidBodyComponent(entityId)
			: nullptr;
		const RigidBodyType bodyType = rigidBody ? rigidBody->Type : RigidBodyType::Static;
		if (collider->Shape == ColliderShape::Plane)
		{
			const physx::PxTransform planeTransform = ToPlaneTransform(transform->WorldTransform, *collider);
			const physx::PxVec3 normal = planeTransform.q.rotate(physx::PxVec3(0.0f, 1.0f, 0.0f));
			const float distance = -normal.dot(planeTransform.p);
			entry.Actor = physx::PxCreatePlane(*physics, physx::PxPlane(normal, distance), *entry.Material);
			if (entry.Actor)
			{
				entry.Actor->userData = reinterpret_cast<void*>(static_cast<uintptr_t>(entityId));
			}
			return entry;
		}

		physx::PxTransform shapeLocalPose;
		const physx::PxGeometryHolder geometry = CreateGeometry(*collider, transform->WorldTransform, shapeLocalPose);
		physx::PxRigidActor* actor = nullptr;
		if (bodyType == RigidBodyType::Dynamic || bodyType == RigidBodyType::Kinematic)
		{
			physx::PxRigidDynamic* dynamicActor = physics->createRigidDynamic(ToPxTransform(transform->WorldTransform));
			if (!dynamicActor)
			{
				return entry;
			}

			dynamicActor->setRigidBodyFlag(physx::PxRigidBodyFlag::eKINEMATIC, bodyType == RigidBodyType::Kinematic);
			if (rigidBody)
			{
				dynamicActor->setActorFlag(physx::PxActorFlag::eDISABLE_GRAVITY, !rigidBody->UseGravity);
				dynamicActor->setLinearDamping((std::max)(rigidBody->LinearDamping, 0.0f));
				dynamicActor->setAngularDamping((std::max)(rigidBody->AngularDamping, 0.0f));
				dynamicActor->setLinearVelocity(ToPxVec3(rigidBody->LinearVelocity));
				dynamicActor->setAngularVelocity(ToPxVec3(rigidBody->AngularVelocity));
			}
			actor = dynamicActor;
		}
		else
		{
			actor = physics->createRigidStatic(ToPxTransform(transform->WorldTransform));
		}

		if (!actor)
		{
			return entry;
		}

		physx::PxShape* shape = physics->createShape(geometry.any(), *entry.Material, true);
		if (!shape)
		{
			actor->release();
			return entry;
		}
		shape->setLocalPose(shapeLocalPose);
		shape->setFlag(physx::PxShapeFlag::eSIMULATION_SHAPE, !collider->IsTrigger);
		shape->setFlag(physx::PxShapeFlag::eTRIGGER_SHAPE, collider->IsTrigger);
		actor->attachShape(*shape);
		shape->release();

		if (physx::PxRigidDynamic* dynamicActor = actor->is<physx::PxRigidDynamic>();
			dynamicActor && bodyType == RigidBodyType::Dynamic)
		{
			const float mass = rigidBody ? Positive(rigidBody->Mass, 1.0f) : 1.0f;
			physx::PxRigidBodyExt::updateMassAndInertia(*dynamicActor, mass);
		}

		actor->userData = reinterpret_cast<void*>(static_cast<uintptr_t>(entityId));
		entry.Actor = actor;
		return entry;
	}
}
