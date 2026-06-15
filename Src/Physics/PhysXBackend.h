#pragma once

#include <DirectXMath.h>

#include <memory>

namespace physx
{
	class PxFoundation;
	class PxPhysics;
	class PxScene;
	class PxMaterial;
}

namespace Physics
{
	class PhysXBackend
	{
	public:
		PhysXBackend();
		~PhysXBackend();

		PhysXBackend(const PhysXBackend&) = delete;
		PhysXBackend& operator=(const PhysXBackend&) = delete;
		PhysXBackend(PhysXBackend&&) noexcept = delete;
		PhysXBackend& operator=(PhysXBackend&&) noexcept = delete;

		[[nodiscard]] bool Initialize(const DirectX::XMFLOAT3& gravity);
		void Shutdown();
		void SetGravity(const DirectX::XMFLOAT3& gravity) const;

		[[nodiscard]] bool IsInitialized() const noexcept;
		[[nodiscard]] physx::PxPhysics* GetPhysics() const noexcept;
		[[nodiscard]] physx::PxScene* GetScene() const noexcept;
		[[nodiscard]] physx::PxMaterial* GetDefaultMaterial() const noexcept;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
