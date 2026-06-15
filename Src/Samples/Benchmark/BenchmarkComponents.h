#pragma once

#include "Samples/Benchmark/BenchmarkTypes.h"

#include <DirectXMath.h>

namespace Samples::Benchmark
{
	struct EcsTransformComponent
	{
		DirectX::XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT4 Rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
		DirectX::XMFLOAT3 Scale = { 1.0f, 1.0f, 1.0f };
	};

	struct VelocityComponent
	{
		DirectX::XMFLOAT3 Linear = { 0.0f, 0.0f, 0.0f };
	};

	struct BoundsComponent
	{
		DirectX::XMFLOAT3 LocalMin = { -0.5f, -0.5f, -0.5f };
		DirectX::XMFLOAT3 LocalMax = { 0.5f, 0.5f, 0.5f };
		DirectX::XMFLOAT3 WorldMin = { -0.5f, -0.5f, -0.5f };
		DirectX::XMFLOAT3 WorldMax = { 0.5f, 0.5f, 0.5f };
	};

	struct RenderableComponent
	{
		BenchmarkObjectType ObjectType = BenchmarkObjectType::Primitive;
		DirectX::XMFLOAT4 Tint = { 1.0f, 1.0f, 1.0f, 1.0f };
	};

	struct SpinComponent
	{
		float AngularSpeed = 0.0f;
		float Angle = 0.0f;
	};
}
