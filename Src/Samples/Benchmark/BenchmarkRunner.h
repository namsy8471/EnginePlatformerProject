#pragma once

#include "Math/Camera.h"
#include "Samples/Benchmark/BenchmarkTypes.h"
#include "Samples/Benchmark/EcsObjectStore.h"
#include "Samples/Benchmark/NonEcsObjectStore.h"

#include <vector>

namespace Samples::Benchmark
{
	class BenchmarkRunner
	{
	public:
		BenchmarkRunner();

		void Configure(const BenchmarkConfig& config);
		void SetSpawnView(const Camera& camera);
		void Update(float deltaTime);
		void DrawImGui();
		void DrawViewportOverlay(const Camera& camera, float viewportLeft, float viewportTop, float viewportWidth, float viewportHeight) const;
		void RunSweep();

		[[nodiscard]] const BenchmarkConfig& GetConfig() const noexcept;
		[[nodiscard]] const BenchmarkStats& GetLastStats() const noexcept;
		[[nodiscard]] const std::vector<BenchmarkScenarioResult>& GetSweepResults() const noexcept;
		[[nodiscard]] uint32_t GetCpuMaterializedObjectCount() const noexcept;
		[[nodiscard]] bool IsConfigured() const noexcept;

	private:
		[[nodiscard]] std::vector<BenchmarkSpawnData> CreateSpawnData(const BenchmarkConfig& config) const;
		[[nodiscard]] uint32_t GetCpuMaterializedObjectCount(const BenchmarkConfig& config) const noexcept;
		[[nodiscard]] BenchmarkStats RunScenario(const BenchmarkConfig& config) const;
		void RebuildStores();
		void LogScenarioResult(const BenchmarkScenarioResult& result) const;

		struct SpawnView
		{
			bool Enabled = false;
			DirectX::XMFLOAT3 Eye = { 0.0f, 0.0f, -5.0f };
			DirectX::XMFLOAT3 Forward = { 0.0f, 0.0f, 1.0f };
			DirectX::XMFLOAT3 Right = { 1.0f, 0.0f, 0.0f };
			DirectX::XMFLOAT3 Up = { 0.0f, 1.0f, 0.0f };
			float FovY = DirectX::XM_PIDIV4;
			float Aspect = 16.0f / 9.0f;
			float NearDistance = 18.0f;
			float FarDistance = 120.0f;
		};

		BenchmarkConfig m_Config;
		SpawnView m_SpawnView;
		NonEcsObjectStore m_NonEcsStore;
		EcsObjectStore m_EcsStore;
		std::vector<BenchmarkSpawnData> m_SpawnData;
		std::vector<BenchmarkRenderInstance> m_RenderInstances;
		BenchmarkStats m_LastStats;
		std::vector<BenchmarkScenarioResult> m_SweepResults;
		bool m_ShowViewportDots = true;
		bool m_IsConfigured = false;
	};
}
