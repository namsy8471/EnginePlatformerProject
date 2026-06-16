#pragma once

#include "Jobs/JobSystem.h"
#include "Memory/StdAllocator.h"
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
		void UpdateSimulation(float deltaTime, Jobs::JobSystem* jobSystem);
		void UpdateMovement(float deltaTime);
		void UpdateSpin(float deltaTime);
		void UpdateBounds();
		uint32_t CollectRenderInstances(std::vector<BenchmarkRenderInstance>& instances);
		uint32_t CollectRenderInstances(std::vector<BenchmarkRenderInstance>& instances, Jobs::JobSystem* jobSystem) const;
		void Clear();

		[[nodiscard]] size_t Size() const noexcept;

	private:
		static constexpr size_t ChunkCapacity = 1024;

		struct Chunk
		{
			Memory::Vector<EcsTransformComponent, Memory::MemoryTag::ECS> Transforms;
			Memory::Vector<VelocityComponent, Memory::MemoryTag::ECS> Velocities;
			Memory::Vector<BoundsComponent, Memory::MemoryTag::ECS> Bounds;
			Memory::Vector<RenderableComponent, Memory::MemoryTag::ECS> Renderables;
			Memory::Vector<SpinComponent, Memory::MemoryTag::ECS> Spins;

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

		Memory::Vector<Chunk, Memory::MemoryTag::ECS> m_Chunks;
		size_t m_Size = 0;
	};
}
