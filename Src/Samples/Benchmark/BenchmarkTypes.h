#pragma once

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace Samples::Benchmark
{
	enum class SampleMode
	{
		ProjectScene = 0,
		SpiderSample = 1,
		EcsBenchmark = 2
	};

	enum class BenchmarkMode
	{
		NonEcs = 0,
		Ecs = 1
	};

	enum class BenchmarkObjectType
	{
		Primitive = 0,
		Spider = 1
	};

	inline constexpr std::array<uint32_t, 6> kBenchmarkObjectCounts = { 100, 1000, 10000, 100000, 1000000, 10000000 };

	struct BenchmarkConfig
	{
		BenchmarkMode Mode = BenchmarkMode::NonEcs;
		BenchmarkObjectType ObjectType = BenchmarkObjectType::Primitive;
		uint32_t ObjectCount = 1000;
		uint32_t WarmupFrames = 30;
		uint32_t SampleFrames = 120;
		uint32_t Seed = 1337;
	};

	struct BenchmarkStats
	{
		double UpdateMs = 0.0;
		double RenderSubmitMs = 0.0;
		double FrameMs = 0.0;
		double Fps = 0.0;
		uint32_t VisibleObjectCount = 0;
	};

	struct BenchmarkScenarioResult
	{
		BenchmarkConfig Config;
		BenchmarkStats Stats;
	};

	struct BenchmarkSpawnData
	{
		DirectX::XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT3 Velocity = { 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT3 Scale = { 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT4 Tint = { 1.0f, 1.0f, 1.0f, 1.0f };
		float AngularSpeed = 0.0f;
		float SpinAngle = 0.0f;
		DirectX::XMFLOAT3 LocalBoundsMin = { -0.5f, -0.5f, -0.5f };
		DirectX::XMFLOAT3 LocalBoundsMax = { 0.5f, 0.5f, 0.5f };
	};

	struct BenchmarkRenderInstance
	{
		DirectX::XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT4 Rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
		DirectX::XMFLOAT3 Scale = { 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT4 Tint = { 1.0f, 1.0f, 1.0f, 1.0f };
		BenchmarkObjectType ObjectType = BenchmarkObjectType::Primitive;
	};

	[[nodiscard]] constexpr std::string_view ToString(SampleMode mode) noexcept
	{
		switch (mode)
		{
		case SampleMode::ProjectScene:
			return "Project Scene";
		case SampleMode::SpiderSample:
			return "Spider Sample";
		case SampleMode::EcsBenchmark:
			return "ECS Benchmark";
		default:
			return "Unknown";
		}
	}

	[[nodiscard]] constexpr std::string_view ToString(BenchmarkMode mode) noexcept
	{
		switch (mode)
		{
		case BenchmarkMode::NonEcs:
			return "Non-ECS";
		case BenchmarkMode::Ecs:
			return "ECS";
		default:
			return "Unknown";
		}
	}

	[[nodiscard]] constexpr std::string_view ToString(BenchmarkObjectType type) noexcept
	{
		switch (type)
		{
		case BenchmarkObjectType::Primitive:
			return "Primitive";
		case BenchmarkObjectType::Spider:
			return "Spider";
		default:
			return "Unknown";
		}
	}
}
