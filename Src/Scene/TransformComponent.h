#pragma once

#include "Math/Transform.h"

struct TransformComponent
{
	Math::Transform LocalTransform = Math::Transform::Identity();
	Math::Transform WorldTransform = Math::Transform::Identity();

	void SetLocalTransform(const Math::Transform& transform) noexcept
	{
		LocalTransform = transform;
	}

	void SetWorldTransform(const Math::Transform& transform) noexcept
	{
		WorldTransform = transform;
	}

	void UpdateWorld(const Math::Transform& parentTransform = Math::Transform::Identity()) noexcept
	{
		WorldTransform = LocalTransform * parentTransform;
	}

	[[nodiscard]] DirectX::XMMATRIX GetWorldXmMatrix() const noexcept
	{
		return WorldTransform.ToXmMatrix();
	}

	[[nodiscard]] DirectX::XMFLOAT4X4 GetWorldMatrix() const noexcept
	{
		return WorldTransform.ToMatrix();
	}
};
