#pragma once

#include "Samples/Benchmark/BenchmarkComponents.h"

#include <span>
#include <vector>

namespace Samples::Benchmark
{
	struct BenchmarkObject
	{
		EcsTransformComponent Transform;
		VelocityComponent Velocity;
		BoundsComponent Bounds;
		RenderableComponent Renderable;
		SpinComponent Spin;
	};

	class NonEcsObjectStore
	{
	public:
		void Build(std::span<const BenchmarkSpawnData> spawnData, BenchmarkObjectType objectType);
		void UpdateSimulation(float deltaTime);
		void UpdateMovement(float deltaTime);
		void UpdateSpin(float deltaTime);
		void UpdateBounds();
		uint32_t CollectRenderInstances(std::vector<BenchmarkRenderInstance>& instances) const;
		void Clear();

		[[nodiscard]] size_t Size() const noexcept;
		[[nodiscard]] const std::vector<BenchmarkObject>& Objects() const noexcept;

	private:
		std::vector<BenchmarkObject> m_Objects;
	};
}
