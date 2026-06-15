#include "NonEcsObjectStore.h"

#include "Samples/Benchmark/BenchmarkSimulation.h"

namespace Samples::Benchmark
{
	void NonEcsObjectStore::Build(std::span<const BenchmarkSpawnData> spawnData, BenchmarkObjectType objectType)
	{
		m_Objects.clear();
		m_Objects.reserve(spawnData.size());

		for (const BenchmarkSpawnData& spawn : spawnData)
		{
			BenchmarkObject object;
			object.Transform.Position = spawn.Position;
			object.Transform.Scale = spawn.Scale;
			object.Velocity.Linear = spawn.Velocity;
			object.Bounds.LocalMin = spawn.LocalBoundsMin;
			object.Bounds.LocalMax = spawn.LocalBoundsMax;
			object.Renderable.ObjectType = objectType;
			object.Renderable.Tint = spawn.Tint;
			object.Spin.AngularSpeed = spawn.AngularSpeed;
			object.Spin.Angle = spawn.SpinAngle;
			ApplySpin(object.Transform, object.Spin, 0.0f);
			UpdateWorldBounds(object.Transform, object.Bounds);
			m_Objects.push_back(object);
		}
	}

	void NonEcsObjectStore::UpdateSimulation(float deltaTime)
	{
		for (BenchmarkObject& object : m_Objects)
		{
			ApplyMovement(object.Transform, object.Velocity, deltaTime);
			ApplySpin(object.Transform, object.Spin, deltaTime);
			UpdateWorldBounds(object.Transform, object.Bounds);
		}
	}

	void NonEcsObjectStore::UpdateMovement(float deltaTime)
	{
		for (BenchmarkObject& object : m_Objects)
		{
			ApplyMovement(object.Transform, object.Velocity, deltaTime);
		}
	}

	void NonEcsObjectStore::UpdateSpin(float deltaTime)
	{
		for (BenchmarkObject& object : m_Objects)
		{
			ApplySpin(object.Transform, object.Spin, deltaTime);
		}
	}

	void NonEcsObjectStore::UpdateBounds()
	{
		for (BenchmarkObject& object : m_Objects)
		{
			UpdateWorldBounds(object.Transform, object.Bounds);
		}
	}

	uint32_t NonEcsObjectStore::CollectRenderInstances(std::vector<BenchmarkRenderInstance>& instances) const
	{
		instances.clear();
		instances.reserve(m_Objects.size());

		for (const BenchmarkObject& object : m_Objects)
		{
			instances.push_back(MakeRenderInstance(object.Transform, object.Renderable));
		}

		return static_cast<uint32_t>(instances.size());
	}

	void NonEcsObjectStore::Clear()
	{
		m_Objects.clear();
	}

	size_t NonEcsObjectStore::Size() const noexcept
	{
		return m_Objects.size();
	}

	const std::vector<BenchmarkObject>& NonEcsObjectStore::Objects() const noexcept
	{
		return m_Objects;
	}
}
