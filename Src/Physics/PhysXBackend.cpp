#include "Physics/PhysXBackend.h"

#include <PxPhysicsAPI.h>

namespace Physics
{
	namespace
	{
		[[nodiscard]] physx::PxVec3 ToPxVec3(const DirectX::XMFLOAT3& value) noexcept
		{
			return physx::PxVec3(value.x, value.y, value.z);
		}
	}

	struct PhysXBackend::Impl
	{
		physx::PxDefaultAllocator Allocator;
		physx::PxDefaultErrorCallback ErrorCallback;
		physx::PxFoundation* Foundation = nullptr;
		physx::PxPhysics* Physics = nullptr;
		physx::PxDefaultCpuDispatcher* Dispatcher = nullptr;
		physx::PxScene* Scene = nullptr;
		physx::PxMaterial* DefaultMaterial = nullptr;
	};

	PhysXBackend::PhysXBackend()
		: m_Impl(std::make_unique<Impl>())
	{
	}

	PhysXBackend::~PhysXBackend()
	{
		Shutdown();
	}

	bool PhysXBackend::Initialize(const DirectX::XMFLOAT3& gravity)
	{
		if (IsInitialized())
		{
			return true;
		}

		m_Impl->Foundation = PxCreateFoundation(PX_PHYSICS_VERSION, m_Impl->Allocator, m_Impl->ErrorCallback);
		if (!m_Impl->Foundation)
		{
			return false;
		}

		m_Impl->Physics = PxCreatePhysics(PX_PHYSICS_VERSION, *m_Impl->Foundation, physx::PxTolerancesScale(), false, nullptr);
		if (!m_Impl->Physics)
		{
			Shutdown();
			return false;
		}

		m_Impl->Dispatcher = physx::PxDefaultCpuDispatcherCreate(2);
		if (!m_Impl->Dispatcher)
		{
			Shutdown();
			return false;
		}

		physx::PxSceneDesc sceneDesc(m_Impl->Physics->getTolerancesScale());
		sceneDesc.gravity = ToPxVec3(gravity);
		sceneDesc.cpuDispatcher = m_Impl->Dispatcher;
		sceneDesc.filterShader = physx::PxDefaultSimulationFilterShader;
		sceneDesc.flags |= physx::PxSceneFlag::eENABLE_ACTIVE_ACTORS;
		m_Impl->Scene = m_Impl->Physics->createScene(sceneDesc);
		if (!m_Impl->Scene)
		{
			Shutdown();
			return false;
		}

		m_Impl->DefaultMaterial = m_Impl->Physics->createMaterial(0.5f, 0.5f, 0.05f);
		if (!m_Impl->DefaultMaterial)
		{
			Shutdown();
			return false;
		}

		return true;
	}

	void PhysXBackend::Shutdown()
	{
		if (m_Impl->DefaultMaterial)
		{
			m_Impl->DefaultMaterial->release();
			m_Impl->DefaultMaterial = nullptr;
		}
		if (m_Impl->Scene)
		{
			m_Impl->Scene->release();
			m_Impl->Scene = nullptr;
		}
		if (m_Impl->Dispatcher)
		{
			m_Impl->Dispatcher->release();
			m_Impl->Dispatcher = nullptr;
		}
		if (m_Impl->Physics)
		{
			m_Impl->Physics->release();
			m_Impl->Physics = nullptr;
		}
		if (m_Impl->Foundation)
		{
			m_Impl->Foundation->release();
			m_Impl->Foundation = nullptr;
		}
	}

	void PhysXBackend::SetGravity(const DirectX::XMFLOAT3& gravity) const
	{
		if (m_Impl->Scene)
		{
			m_Impl->Scene->setGravity(ToPxVec3(gravity));
		}
	}

	bool PhysXBackend::IsInitialized() const noexcept
	{
		return m_Impl->Foundation && m_Impl->Physics && m_Impl->Scene && m_Impl->DefaultMaterial;
	}

	physx::PxPhysics* PhysXBackend::GetPhysics() const noexcept
	{
		return m_Impl->Physics;
	}

	physx::PxScene* PhysXBackend::GetScene() const noexcept
	{
		return m_Impl->Scene;
	}

	physx::PxMaterial* PhysXBackend::GetDefaultMaterial() const noexcept
	{
		return m_Impl->DefaultMaterial;
	}
}
