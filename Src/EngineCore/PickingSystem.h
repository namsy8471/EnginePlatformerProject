#pragma once

#include "Scene.h"
#include "Math/Camera.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace PickingSystem
{
	struct PickRay
	{
		DirectX::XMVECTOR Origin = DirectX::XMVectorZero();
		DirectX::XMVECTOR Direction = DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
	};

	[[nodiscard]] inline PickRay CreatePickRay(float mouseX, float mouseY, float viewportWidth, float viewportHeight, const Camera& camera)
	{
		const float ndcX = (2.0f * mouseX / viewportWidth) - 1.0f;
		const float ndcY = 1.0f - (2.0f * mouseY / viewportHeight);
		const DirectX::XMMATRIX inverseProjection = DirectX::XMMatrixInverse(nullptr, camera.GetProjectionMatrix());
		const DirectX::XMMATRIX inverseView = DirectX::XMMatrixInverse(nullptr, camera.GetViewMatrix());

		const DirectX::XMVECTOR nearPoint = DirectX::XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);
		const DirectX::XMVECTOR farPoint = DirectX::XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);

		const DirectX::XMVECTOR nearView = DirectX::XMVector3TransformCoord(nearPoint, inverseProjection);
		const DirectX::XMVECTOR farView = DirectX::XMVector3TransformCoord(farPoint, inverseProjection);
		const DirectX::XMVECTOR origin = DirectX::XMVector3TransformCoord(nearView, inverseView);
		const DirectX::XMVECTOR farWorld = DirectX::XMVector3TransformCoord(farView, inverseView);

		PickRay ray = {};
		ray.Origin = origin;
		ray.Direction = DirectX::XMVector3Normalize(farWorld - origin);
		return ray;
	}

	inline void ComputeWorldAabb(const DirectX::XMFLOAT3& localMin, const DirectX::XMFLOAT3& localMax, const DirectX::XMFLOAT4X4& worldMatrix, DirectX::XMFLOAT3& worldMin, DirectX::XMFLOAT3& worldMax)
	{
		const DirectX::XMFLOAT3 corners[] = {
			{ localMin.x, localMin.y, localMin.z },
			{ localMin.x, localMin.y, localMax.z },
			{ localMin.x, localMax.y, localMin.z },
			{ localMin.x, localMax.y, localMax.z },
			{ localMax.x, localMin.y, localMin.z },
			{ localMax.x, localMin.y, localMax.z },
			{ localMax.x, localMax.y, localMin.z },
			{ localMax.x, localMax.y, localMax.z }
		};

		worldMin = {
			(std::numeric_limits<float>::max)(),
			(std::numeric_limits<float>::max)(),
			(std::numeric_limits<float>::max)()
		};
		worldMax = {
			(std::numeric_limits<float>::lowest)(),
			(std::numeric_limits<float>::lowest)(),
			(std::numeric_limits<float>::lowest)()
		};

		for (const auto& corner : corners)
		{
			const DirectX::XMFLOAT3 transformedCorner = Math::TransformPoint(corner, worldMatrix);
			worldMin.x = (std::min)(worldMin.x, transformedCorner.x);
			worldMin.y = (std::min)(worldMin.y, transformedCorner.y);
			worldMin.z = (std::min)(worldMin.z, transformedCorner.z);
			worldMax.x = (std::max)(worldMax.x, transformedCorner.x);
			worldMax.y = (std::max)(worldMax.y, transformedCorner.y);
			worldMax.z = (std::max)(worldMax.z, transformedCorner.z);
		}
	}

	[[nodiscard]] inline bool IntersectRayAabb(DirectX::FXMVECTOR rayOrigin, DirectX::FXMVECTOR rayDirection, const DirectX::XMFLOAT3& aabbMin, const DirectX::XMFLOAT3& aabbMax)
	{
		DirectX::XMFLOAT3 origin = {};
		DirectX::XMFLOAT3 direction = {};
		DirectX::XMStoreFloat3(&origin, rayOrigin);
		DirectX::XMStoreFloat3(&direction, rayDirection);

		float tMin = 0.0f;
		float tMax = (std::numeric_limits<float>::max)();
		const float epsilon = 1.0e-6f;

		const float originComponents[] = { origin.x, origin.y, origin.z };
		const float directionComponents[] = { direction.x, direction.y, direction.z };
		const float minComponents[] = { aabbMin.x, aabbMin.y, aabbMin.z };
		const float maxComponents[] = { aabbMax.x, aabbMax.y, aabbMax.z };

		for (size_t axis = 0; axis < 3; ++axis)
		{
			if (std::fabs(directionComponents[axis]) < epsilon)
			{
				if (originComponents[axis] < minComponents[axis] || originComponents[axis] > maxComponents[axis])
				{
					return false;
				}
				continue;
			}

			const float inverseDirection = 1.0f / directionComponents[axis];
			float t1 = (minComponents[axis] - originComponents[axis]) * inverseDirection;
			float t2 = (maxComponents[axis] - originComponents[axis]) * inverseDirection;

			if (t1 > t2)
			{
				std::swap(t1, t2);
			}

			tMin = (std::max)(tMin, t1);
			tMax = (std::min)(tMax, t2);
			if (tMin > tMax)
			{
				return false;
			}
		}

		return tMax >= 0.0f;
	}

	[[nodiscard]] inline bool TryPickEntityAabb(const Scene& scene, EntityId entityId, const Camera& camera, float mouseX, float mouseY, float viewportWidth, float viewportHeight)
	{
		const Asset::StaticMeshAsset* meshAsset = scene.GetMeshAsset(entityId);
		const TransformComponent* transform = scene.GetTransformComponent(entityId);
		const BoundsComponent* bounds = scene.GetBoundsComponent(entityId);
		if (!meshAsset || !transform || !bounds || viewportWidth <= 0.0f || viewportHeight <= 0.0f)
		{
			return false;
		}

		const PickRay ray = CreatePickRay(mouseX, mouseY, viewportWidth, viewportHeight, camera);
		DirectX::XMFLOAT3 worldBoundsMin = {};
		DirectX::XMFLOAT3 worldBoundsMax = {};
		ComputeWorldAabb(bounds->LocalMin, bounds->LocalMax, transform->GetWorldMatrix(), worldBoundsMin, worldBoundsMax);
		return IntersectRayAabb(ray.Origin, ray.Direction, worldBoundsMin, worldBoundsMax);
	}
}
