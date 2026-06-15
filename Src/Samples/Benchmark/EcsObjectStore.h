#pragma once

#include "Samples/Benchmark/BenchmarkComponents.h"

#include <span>
#include <vector>

namespace Samples::Benchmark
{
	class EcsObjectStore
	{
	public:
		void Build(std::span<const BenchmarkSpawnData> spawnData, BenchmarkObjectType objectType);
		void UpdateSimulation(float deltaTime);
		void UpdateMovement(float deltaTime);
		void UpdateSpin(float deltaTime);
		void UpdateBounds();
		uint32_t CollectRenderInstances(std::vector<BenchmarkRenderInstance>& instances);
		void Clear();

		[[nodiscard]] size_t Size() const noexcept;

	private:
		static constexpr size_t ChunkCapacity = 1024;

		struct Chunk
		{
			std::vector<EcsTransformComponent> Transforms;
			std::vector<VelocityComponent> Velocities;
			std::vector<BoundsComponent> Bounds;
			std::vector<RenderableComponent> Renderables;
			std::vector<SpinComponent> Spins;

			void Reserve(size_t capacity);
			void Push(
				const EcsTransformComponent& transform,
				const VelocityComponent& velocity,
				const BoundsComponent& bounds,
				const RenderableComponent& renderable,
				const SpinComponent& spin);

			[[nodiscard]] bool IsFull() const noexcept;
			[[nodiscard]] size_t Size() const noexcept;
		};

		[[nodiscard]] Chunk& AddChunk();

		std::vector<Chunk> m_Chunks;
		size_t m_Size = 0;
	};
}
