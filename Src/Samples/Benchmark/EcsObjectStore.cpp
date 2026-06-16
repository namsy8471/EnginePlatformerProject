#include "EcsObjectStore.h"

#include "Samples/Benchmark/BenchmarkSimulation.h"

#include <utility>

namespace Samples::Benchmark
{
	void EcsObjectStore::Chunk::Reserve(size_t capacity)
	{
		Transforms.reserve(capacity);
		Velocities.reserve(capacity);
		Bounds.reserve(capacity);
		Renderables.reserve(capacity);
		Spins.reserve(capacity);
	}

	void EcsObjectStore::Chunk::Push(
		const EcsTransformComponent& transform,
		const VelocityComponent& velocity,
		const BoundsComponent& bounds,
		const RenderableComponent& renderable,
		const SpinComponent& spin)
	{
		Transforms.push_back(transform);
		Velocities.push_back(velocity);
		Bounds.push_back(bounds);
		Renderables.push_back(renderable);
		Spins.push_back(spin);
	}

	bool EcsObjectStore::Chunk::IsFull() const noexcept
	{
		return Transforms.size() >= ChunkCapacity;
	}

	size_t EcsObjectStore::Chunk::Size() const noexcept
	{
		return Transforms.size();
	}

	EcsObjectStore::Chunk& EcsObjectStore::AddChunk()
	{
		Chunk chunk;
		chunk.Reserve(ChunkCapacity);
		m_Chunks.push_back(std::move(chunk));
		return m_Chunks.back();
	}

	void EcsObjectStore::Build(std::span<const BenchmarkSpawnData> spawnData, BenchmarkObjectType objectType)
	{
		Clear();
		m_Chunks.reserve((spawnData.size() + ChunkCapacity - 1) / ChunkCapacity);

		for (const BenchmarkSpawnData& spawn : spawnData)
		{
			EcsTransformComponent transform;
			transform.Position = spawn.Position;
			transform.Scale = spawn.Scale;

			VelocityComponent velocity;
			velocity.Linear = spawn.Velocity;

			BoundsComponent bounds;
			bounds.LocalMin = spawn.LocalBoundsMin;
			bounds.LocalMax = spawn.LocalBoundsMax;

			RenderableComponent renderable;
			renderable.ObjectType = objectType;
			renderable.Tint = spawn.Tint;

			SpinComponent spin;
			spin.AngularSpeed = spawn.AngularSpeed;
			spin.Angle = spawn.SpinAngle;

			ApplySpin(transform, spin, 0.0f);
			UpdateWorldBounds(transform, bounds);

			Chunk& chunk = m_Chunks.empty() || m_Chunks.back().IsFull() ? AddChunk() : m_Chunks.back();
			chunk.Push(transform, velocity, bounds, renderable, spin);
			++m_Size;
		}
	}

	void EcsObjectStore::UpdateSimulation(float deltaTime)
	{
		for (Chunk& chunk : m_Chunks)
		{
			const size_t count = chunk.Size();
			EcsTransformComponent* transforms = chunk.Transforms.data();
			const VelocityComponent* velocities = chunk.Velocities.data();
			BoundsComponent* bounds = chunk.Bounds.data();
			SpinComponent* spins = chunk.Spins.data();

			for (size_t i = 0; i < count; ++i)
			{
				ApplyMovement(transforms[i], velocities[i], deltaTime);
				ApplySpin(transforms[i], spins[i], deltaTime);
				UpdateWorldBounds(transforms[i], bounds[i]);
			}
		}
	}

	void EcsObjectStore::UpdateSimulation(float deltaTime, Jobs::JobSystem* jobSystem)
	{
		if (!jobSystem || m_Chunks.size() < 2 || m_Size < 2048)
		{
			UpdateSimulation(deltaTime);
			return;
		}

		jobSystem->RunParallelFor(
			m_Chunks.size(),
			1,
			"ECS Benchmark UpdateSimulation",
			[this, deltaTime](size_t begin, size_t end, Jobs::JobContext&)
			{
				for (size_t chunkIndex = begin; chunkIndex < end; ++chunkIndex)
				{
					Chunk& chunk = m_Chunks[chunkIndex];
					const size_t count = chunk.Size();
					EcsTransformComponent* transforms = chunk.Transforms.data();
					const VelocityComponent* velocities = chunk.Velocities.data();
					BoundsComponent* bounds = chunk.Bounds.data();
					SpinComponent* spins = chunk.Spins.data();

					for (size_t i = 0; i < count; ++i)
					{
						ApplyMovement(transforms[i], velocities[i], deltaTime);
						ApplySpin(transforms[i], spins[i], deltaTime);
						UpdateWorldBounds(transforms[i], bounds[i]);
					}
				}
			});
	}

	void EcsObjectStore::UpdateMovement(float deltaTime)
	{
		for (Chunk& chunk : m_Chunks)
		{
			const size_t count = chunk.Size();
			EcsTransformComponent* transforms = chunk.Transforms.data();
			const VelocityComponent* velocities = chunk.Velocities.data();

			for (size_t i = 0; i < count; ++i)
			{
				ApplyMovement(transforms[i], velocities[i], deltaTime);
			}
		}
	}

	void EcsObjectStore::UpdateSpin(float deltaTime)
	{
		for (Chunk& chunk : m_Chunks)
		{
			const size_t count = chunk.Size();
			EcsTransformComponent* transforms = chunk.Transforms.data();
			SpinComponent* spins = chunk.Spins.data();

			for (size_t i = 0; i < count; ++i)
			{
				ApplySpin(transforms[i], spins[i], deltaTime);
			}
		}
	}

	void EcsObjectStore::UpdateBounds()
	{
		for (Chunk& chunk : m_Chunks)
		{
			const size_t count = chunk.Size();
			const EcsTransformComponent* transforms = chunk.Transforms.data();
			BoundsComponent* bounds = chunk.Bounds.data();

			for (size_t i = 0; i < count; ++i)
			{
				UpdateWorldBounds(transforms[i], bounds[i]);
			}
		}
	}

	uint32_t EcsObjectStore::CollectRenderInstances(std::vector<BenchmarkRenderInstance>& instances)
	{
		instances.clear();
		instances.reserve(m_Size);

		for (const Chunk& chunk : m_Chunks)
		{
			const size_t count = chunk.Size();
			const EcsTransformComponent* transforms = chunk.Transforms.data();
			const RenderableComponent* renderables = chunk.Renderables.data();

			for (size_t i = 0; i < count; ++i)
			{
				instances.push_back(MakeRenderInstance(transforms[i], renderables[i]));
			}
		}

		return static_cast<uint32_t>(instances.size());
	}

	uint32_t EcsObjectStore::CollectRenderInstances(std::vector<BenchmarkRenderInstance>& instances, Jobs::JobSystem* jobSystem) const
	{
		if (!jobSystem || m_Chunks.size() < 2 || m_Size < 2048)
		{
			instances.clear();
			instances.reserve(m_Size);
			for (const Chunk& chunk : m_Chunks)
			{
				const size_t count = chunk.Size();
				const EcsTransformComponent* transforms = chunk.Transforms.data();
				const RenderableComponent* renderables = chunk.Renderables.data();

				for (size_t i = 0; i < count; ++i)
				{
					instances.push_back(MakeRenderInstance(transforms[i], renderables[i]));
				}
			}
			return static_cast<uint32_t>(instances.size());
		}

		std::vector<size_t> chunkOffsets(m_Chunks.size(), 0);
		size_t runningOffset = 0;
		for (size_t chunkIndex = 0; chunkIndex < m_Chunks.size(); ++chunkIndex)
		{
			chunkOffsets[chunkIndex] = runningOffset;
			runningOffset += m_Chunks[chunkIndex].Size();
		}

		instances.clear();
		instances.resize(runningOffset);
		jobSystem->RunParallelFor(
			m_Chunks.size(),
			1,
			"ECS Benchmark CollectRenderInstances",
			[this, &instances, &chunkOffsets](size_t begin, size_t end, Jobs::JobContext&)
			{
				for (size_t chunkIndex = begin; chunkIndex < end; ++chunkIndex)
				{
					const Chunk& chunk = m_Chunks[chunkIndex];
					const size_t count = chunk.Size();
					const size_t outputOffset = chunkOffsets[chunkIndex];
					const EcsTransformComponent* transforms = chunk.Transforms.data();
					const RenderableComponent* renderables = chunk.Renderables.data();

					for (size_t i = 0; i < count; ++i)
					{
						instances[outputOffset + i] = MakeRenderInstance(transforms[i], renderables[i]);
					}
				}
			});
		return static_cast<uint32_t>(instances.size());
	}

	void EcsObjectStore::Clear()
	{
		m_Chunks.clear();
		m_Size = 0;
	}

	size_t EcsObjectStore::Size() const noexcept
	{
		return m_Size;
	}
}
