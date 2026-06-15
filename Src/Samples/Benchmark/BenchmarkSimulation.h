#pragma once

#include "Samples/Benchmark/BenchmarkComponents.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>

namespace Samples::Benchmark
{
	inline void ApplyMovement(EcsTransformComponent& transform, const VelocityComponent& velocity, float deltaTime) noexcept
	{
		transform.Position.x += velocity.Linear.x * deltaTime;
		transform.Position.y += velocity.Linear.y * deltaTime;
		transform.Position.z += velocity.Linear.z * deltaTime;
	}

	inline void ApplySpin(EcsTransformComponent& transform, SpinComponent& spin, float deltaTime) noexcept
	{
		constexpr float twoPi = 6.28318530717958647692f;
		spin.Angle = std::fmod(spin.Angle + spin.AngularSpeed * deltaTime, twoPi);

		const DirectX::XMVECTOR rotation = DirectX::XMQuaternionRotationRollPitchYaw(0.0f, spin.Angle, 0.0f);
		DirectX::XMStoreFloat4(&transform.Rotation, rotation);
	}

	inline void UpdateWorldBounds(const EcsTransformComponent& transform, BoundsComponent& bounds) noexcept
	{
		const DirectX::XMFLOAT3 scaledMin = {
			bounds.LocalMin.x * transform.Scale.x,
			bounds.LocalMin.y * transform.Scale.y,
			bounds.LocalMin.z * transform.Scale.z
		};
		const DirectX::XMFLOAT3 scaledMax = {
			bounds.LocalMax.x * transform.Scale.x,
			bounds.LocalMax.y * transform.Scale.y,
			bounds.LocalMax.z * transform.Scale.z
		};

		bounds.WorldMin = {
			transform.Position.x + (std::min)(scaledMin.x, scaledMax.x),
			transform.Position.y + (std::min)(scaledMin.y, scaledMax.y),
			transform.Position.z + (std::min)(scaledMin.z, scaledMax.z)
		};
		bounds.WorldMax = {
			transform.Position.x + (std::max)(scaledMin.x, scaledMax.x),
			transform.Position.y + (std::max)(scaledMin.y, scaledMax.y),
			transform.Position.z + (std::max)(scaledMin.z, scaledMax.z)
		};
	}

	[[nodiscard]] inline BenchmarkRenderInstance MakeRenderInstance(
		const EcsTransformComponent& transform,
		const RenderableComponent& renderable) noexcept
	{
		return BenchmarkRenderInstance{
			.Position = transform.Position,
			.Rotation = transform.Rotation,
			.Scale = transform.Scale,
			.Tint = renderable.Tint,
			.ObjectType = renderable.ObjectType
		};
	}
}
