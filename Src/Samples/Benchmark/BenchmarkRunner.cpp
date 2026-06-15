#include "BenchmarkRunner.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <random>
#include <sstream>
#include <string>

namespace Samples::Benchmark
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		[[nodiscard]] double ToMilliseconds(Clock::time_point begin, Clock::time_point end) noexcept
		{
			return std::chrono::duration<double, std::milli>(end - begin).count();
		}

		[[nodiscard]] BenchmarkConfig SanitizeConfig(BenchmarkConfig config) noexcept
		{
			if (config.ObjectCount == 0)
			{
				config.ObjectCount = 100;
			}
			config.WarmupFrames = (std::max)(config.WarmupFrames, 0u);
			config.SampleFrames = (std::max)(config.SampleFrames, 1u);
			return config;
		}

		[[nodiscard]] constexpr uint32_t MaxCpuMaterializedObjects() noexcept
		{
			return 100000;
		}

		template <typename StoreType>
		[[nodiscard]] BenchmarkStats TickStore(StoreType& store, std::vector<BenchmarkRenderInstance>& renderInstances, float deltaTime)
		{
			const auto frameBegin = Clock::now();

			const auto updateBegin = frameBegin;
			if constexpr (requires(StoreType& updateStore, float updateDeltaTime) { updateStore.UpdateSimulation(updateDeltaTime); })
			{
				store.UpdateSimulation(deltaTime);
			}
			else
			{
				store.UpdateMovement(deltaTime);
				store.UpdateSpin(deltaTime);
				store.UpdateBounds();
			}
			const auto updateEnd = Clock::now();

			const auto renderBegin = updateEnd;
			const uint32_t visibleCount = store.CollectRenderInstances(renderInstances);
			const auto renderEnd = Clock::now();

			BenchmarkStats stats;
			stats.UpdateMs = ToMilliseconds(updateBegin, updateEnd);
			stats.RenderSubmitMs = ToMilliseconds(renderBegin, renderEnd);
			stats.FrameMs = ToMilliseconds(frameBegin, renderEnd);
			stats.Fps = stats.FrameMs > 0.0 ? 1000.0 / stats.FrameMs : 0.0;
			stats.VisibleObjectCount = visibleCount;
			return stats;
		}

		void Accumulate(BenchmarkStats& target, const BenchmarkStats& source) noexcept
		{
			target.UpdateMs += source.UpdateMs;
			target.RenderSubmitMs += source.RenderSubmitMs;
			target.FrameMs += source.FrameMs;
			target.VisibleObjectCount = source.VisibleObjectCount;
		}

		void Average(BenchmarkStats& stats, uint32_t sampleCount) noexcept
		{
			if (sampleCount == 0)
			{
				return;
			}

			const double divisor = static_cast<double>(sampleCount);
			stats.UpdateMs /= divisor;
			stats.RenderSubmitMs /= divisor;
			stats.FrameMs /= divisor;
			stats.Fps = stats.FrameMs > 0.0 ? 1000.0 / stats.FrameMs : 0.0;
		}

		[[nodiscard]] DirectX::XMFLOAT3 NormalizeOrDefault(const DirectX::XMFLOAT3& value, const DirectX::XMFLOAT3& fallback) noexcept
		{
			DirectX::XMVECTOR vector = DirectX::XMLoadFloat3(&value);
			if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(vector)) < 0.000001f)
			{
				return fallback;
			}

			vector = DirectX::XMVector3Normalize(vector);
			DirectX::XMFLOAT3 result = {};
			DirectX::XMStoreFloat3(&result, vector);
			return result;
		}

		[[nodiscard]] DirectX::XMFLOAT3 AddScaled(
			const DirectX::XMFLOAT3& origin,
			const DirectX::XMFLOAT3& forward,
			const DirectX::XMFLOAT3& right,
			const DirectX::XMFLOAT3& up,
			float forwardScale,
			float rightScale,
			float upScale) noexcept
		{
			return {
				origin.x + forward.x * forwardScale + right.x * rightScale + up.x * upScale,
				origin.y + forward.y * forwardScale + right.y * rightScale + up.y * upScale,
				origin.z + forward.z * forwardScale + right.z * rightScale + up.z * upScale
			};
		}

		[[nodiscard]] uint32_t ToImGuiColor(const DirectX::XMFLOAT4& color) noexcept
		{
			return ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, color.w));
		}

		void DrawCurrentStatsTable(const BenchmarkStats& stats)
		{
			constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
			if (!ImGui::BeginTable("BenchmarkCurrentStats", 5, tableFlags))
			{
				return;
			}

			ImGui::TableSetupColumn("Update ms");
			ImGui::TableSetupColumn("Render Collect ms");
			ImGui::TableSetupColumn("CPU Frame ms");
			ImGui::TableSetupColumn("CPU FPS");
			ImGui::TableSetupColumn("Visible");
			ImGui::TableHeadersRow();

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%.3f", stats.UpdateMs);
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%.3f", stats.RenderSubmitMs);
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%.3f", stats.FrameMs);
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%.1f", stats.Fps);
			ImGui::TableSetColumnIndex(4);
			ImGui::Text("%u", stats.VisibleObjectCount);

			ImGui::EndTable();
		}

		void DrawSweepTable(const std::vector<BenchmarkScenarioResult>& results)
		{
			if (results.empty())
			{
				return;
			}

			constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
			if (!ImGui::BeginTable("BenchmarkSweepResults", 8, tableFlags))
			{
				return;
			}

			ImGui::TableSetupColumn("Mode");
			ImGui::TableSetupColumn("Type");
			ImGui::TableSetupColumn("Count");
			ImGui::TableSetupColumn("Update ms");
			ImGui::TableSetupColumn("Render Collect ms");
			ImGui::TableSetupColumn("CPU Frame ms");
			ImGui::TableSetupColumn("CPU FPS");
			ImGui::TableSetupColumn("Visible");
			ImGui::TableHeadersRow();

			for (const BenchmarkScenarioResult& result : results)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(ToString(result.Config.Mode).data());
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(ToString(result.Config.ObjectType).data());
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%u", result.Config.ObjectCount);
				ImGui::TableSetColumnIndex(3);
				ImGui::Text("%.3f", result.Stats.UpdateMs);
				ImGui::TableSetColumnIndex(4);
				ImGui::Text("%.3f", result.Stats.RenderSubmitMs);
				ImGui::TableSetColumnIndex(5);
				ImGui::Text("%.3f", result.Stats.FrameMs);
				ImGui::TableSetColumnIndex(6);
				ImGui::Text("%.1f", result.Stats.Fps);
				ImGui::TableSetColumnIndex(7);
				ImGui::Text("%u", result.Stats.VisibleObjectCount);
			}

			ImGui::EndTable();
		}
	}

	BenchmarkRunner::BenchmarkRunner()
	{
		Configure(m_Config);
	}

	void BenchmarkRunner::Configure(const BenchmarkConfig& config)
	{
		const BenchmarkConfig sanitizedConfig = SanitizeConfig(config);
		const bool needsRebuild =
			!m_IsConfigured
			|| sanitizedConfig.ObjectType != m_Config.ObjectType
			|| sanitizedConfig.ObjectCount != m_Config.ObjectCount
			|| sanitizedConfig.Seed != m_Config.Seed;

		m_Config = sanitizedConfig;
		if (needsRebuild)
		{
			RebuildStores();
		}
	}

	void BenchmarkRunner::SetSpawnView(const Camera& camera)
	{
		m_SpawnView.Enabled = true;
		m_SpawnView.Eye = camera.GetPosition();
		m_SpawnView.Forward = NormalizeOrDefault(camera.GetForward(), { 0.0f, 0.0f, 1.0f });
		m_SpawnView.Right = NormalizeOrDefault(camera.GetRight(), { 1.0f, 0.0f, 0.0f });
		m_SpawnView.Up = NormalizeOrDefault(camera.GetUp(), { 0.0f, 1.0f, 0.0f });
		m_SpawnView.FovY = camera.GetFovY();
		m_SpawnView.Aspect = camera.GetAspect();
		RebuildStores();
	}

	void BenchmarkRunner::Update(float deltaTime)
	{
		if (!m_IsConfigured)
		{
			Configure(m_Config);
		}

		if (m_Config.Mode == BenchmarkMode::NonEcs)
		{
			m_LastStats = TickStore(m_NonEcsStore, m_RenderInstances, deltaTime);
		}
		else
		{
			m_LastStats = TickStore(m_EcsStore, m_RenderInstances, deltaTime);
		}
	}

	void BenchmarkRunner::DrawImGui()
	{
		if (!m_IsConfigured)
		{
			Configure(m_Config);
		}

		BenchmarkConfig editedConfig = m_Config;
		bool configChanged = false;

		int modeIndex = static_cast<int>(editedConfig.Mode);
		if (ImGui::Combo("Benchmark Mode", &modeIndex, "Non-ECS\0ECS\0"))
		{
			editedConfig.Mode = static_cast<BenchmarkMode>(modeIndex);
			configChanged = true;
		}

		int objectTypeIndex = static_cast<int>(editedConfig.ObjectType);
		if (ImGui::Combo("Object Type", &objectTypeIndex, "Primitive\0Spider\0"))
		{
			editedConfig.ObjectType = static_cast<BenchmarkObjectType>(objectTypeIndex);
			configChanged = true;
		}

		int objectCountIndex = 0;
		for (int i = 0; i < static_cast<int>(kBenchmarkObjectCounts.size()); ++i)
		{
			if (kBenchmarkObjectCounts[static_cast<size_t>(i)] == editedConfig.ObjectCount)
			{
				objectCountIndex = i;
				break;
			}
		}

		const char* objectCountItems[] = { "100", "1000", "10000", "100000", "1000000", "10000000" };
		if (ImGui::Combo("Object Count", &objectCountIndex, objectCountItems, static_cast<int>(std::size(objectCountItems))))
		{
			editedConfig.ObjectCount = kBenchmarkObjectCounts[static_cast<size_t>(objectCountIndex)];
			configChanged = true;
		}

		int warmupFrames = static_cast<int>(editedConfig.WarmupFrames);
		if (ImGui::DragInt("Warmup Frames", &warmupFrames, 1.0f, 0, 10000))
		{
			editedConfig.WarmupFrames = static_cast<uint32_t>((std::max)(warmupFrames, 0));
			configChanged = true;
		}

		int sampleFrames = static_cast<int>(editedConfig.SampleFrames);
		if (ImGui::DragInt("Sample Frames", &sampleFrames, 1.0f, 1, 10000))
		{
			editedConfig.SampleFrames = static_cast<uint32_t>((std::max)(sampleFrames, 1));
			configChanged = true;
		}

		if (configChanged)
		{
			Configure(editedConfig);
		}

		if (m_Config.ObjectCount != GetCpuMaterializedObjectCount())
		{
			ImGui::Text("CPU simulated objects: %u / Requested draw count: %u", GetCpuMaterializedObjectCount(), m_Config.ObjectCount);
		}

		if (ImGui::Button("Run Sweep"))
		{
			RunSweep();
		}
		ImGui::SameLine();
		ImGui::Checkbox("Viewport Dots", &m_ShowViewportDots);

		DrawCurrentStatsTable(m_LastStats);
		DrawSweepTable(m_SweepResults);
	}

	void BenchmarkRunner::DrawViewportOverlay(const Camera& camera, float viewportWidth, float viewportHeight) const
	{
		if (!m_ShowViewportDots || m_RenderInstances.empty() || viewportWidth <= 0.0f || viewportHeight <= 0.0f)
		{
			return;
		}

		ImDrawList* drawList = ImGui::GetBackgroundDrawList();
		if (!drawList)
		{
			return;
		}

		const ImVec2 viewportOrigin = ImGui::GetMainViewport()->Pos;
		const DirectX::XMMATRIX viewProjection = camera.GetViewProjectionMatrix();
		const float pointRadius = m_Config.ObjectCount >= 10000 ? 1.0f : 2.0f;

		for (const BenchmarkRenderInstance& instance : m_RenderInstances)
		{
			const DirectX::XMVECTOR worldPosition = DirectX::XMVectorSet(instance.Position.x, instance.Position.y, instance.Position.z, 1.0f);
			const DirectX::XMVECTOR clipPosition = DirectX::XMVector4Transform(worldPosition, viewProjection);
			const float w = DirectX::XMVectorGetW(clipPosition);
			if (w <= 0.0001f)
			{
				continue;
			}

			const float ndcX = DirectX::XMVectorGetX(clipPosition) / w;
			const float ndcY = DirectX::XMVectorGetY(clipPosition) / w;
			if (ndcX < -1.0f || ndcX > 1.0f || ndcY < -1.0f || ndcY > 1.0f)
			{
				continue;
			}

			const float screenX = viewportOrigin.x + (ndcX * 0.5f + 0.5f) * viewportWidth;
			const float screenY = viewportOrigin.y + (-ndcY * 0.5f + 0.5f) * viewportHeight;
			drawList->AddRectFilled(
				ImVec2(screenX - pointRadius, screenY - pointRadius),
				ImVec2(screenX + pointRadius, screenY + pointRadius),
				ToImGuiColor(instance.Tint));
		}
	}

	void BenchmarkRunner::RunSweep()
	{
		m_SweepResults.clear();
		m_SweepResults.reserve(2 * 2 * kBenchmarkObjectCounts.size());

		for (BenchmarkObjectType objectType : { BenchmarkObjectType::Primitive, BenchmarkObjectType::Spider })
		{
			for (uint32_t objectCount : kBenchmarkObjectCounts)
			{
				for (BenchmarkMode mode : { BenchmarkMode::NonEcs, BenchmarkMode::Ecs })
				{
					BenchmarkConfig scenarioConfig = m_Config;
					scenarioConfig.Mode = mode;
					scenarioConfig.ObjectType = objectType;
					scenarioConfig.ObjectCount = objectCount;

					BenchmarkScenarioResult result;
					result.Config = scenarioConfig;
					result.Stats = RunScenario(scenarioConfig);
					m_SweepResults.push_back(result);
					LogScenarioResult(result);
				}
			}
		}
	}

	const BenchmarkConfig& BenchmarkRunner::GetConfig() const noexcept
	{
		return m_Config;
	}

	const BenchmarkStats& BenchmarkRunner::GetLastStats() const noexcept
	{
		return m_LastStats;
	}

	const std::vector<BenchmarkScenarioResult>& BenchmarkRunner::GetSweepResults() const noexcept
	{
		return m_SweepResults;
	}

	uint32_t BenchmarkRunner::GetCpuMaterializedObjectCount() const noexcept
	{
		return GetCpuMaterializedObjectCount(m_Config);
	}

	bool BenchmarkRunner::IsConfigured() const noexcept
	{
		return m_IsConfigured;
	}

	std::vector<BenchmarkSpawnData> BenchmarkRunner::CreateSpawnData(const BenchmarkConfig& config) const
	{
		std::vector<BenchmarkSpawnData> spawnData;
		const uint32_t materializedObjectCount = GetCpuMaterializedObjectCount(config);
		spawnData.reserve(materializedObjectCount);

		const uint32_t typeSalt = config.ObjectType == BenchmarkObjectType::Spider ? 0xA511A11u : 0x51A7E5u;
		std::mt19937 rng(config.Seed ^ (config.ObjectCount * 0x9E3779B9u) ^ typeSalt);
		std::uniform_real_distribution<float> velocityDistribution(-2.0f, 2.0f);
		std::uniform_real_distribution<float> colorDistribution(0.25f, 1.0f);
		std::uniform_real_distribution<float> spinDistribution(0.2f, 2.4f);
		std::uniform_real_distribution<float> angleDistribution(0.0f, 6.28318530717958647692f);
		std::uniform_real_distribution<float> jitterDistribution(0.12f, 0.88f);
		std::uniform_real_distribution<float> depthDistribution(0.0f, 1.0f);
		const bool spiderType = config.ObjectType == BenchmarkObjectType::Spider;
		std::uniform_real_distribution<float> scaleDistribution(spiderType ? 0.025f : 0.35f, spiderType ? 0.08f : 1.25f);
		const uint32_t columnCount = (std::max)(1u, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(materializedObjectCount) * (std::max)(m_SpawnView.Aspect, 0.1f)))));
		const uint32_t rowCount = (std::max)(1u, (materializedObjectCount + columnCount - 1) / columnCount);
		const float tanHalfFovY = std::tan(m_SpawnView.FovY * 0.5f);
		const float tanHalfFovX = tanHalfFovY * (std::max)(m_SpawnView.Aspect, 0.1f);

		for (uint32_t i = 0; i < materializedObjectCount; ++i)
		{
			const float scale = scaleDistribution(rng);
			BenchmarkSpawnData spawn;
			if (m_SpawnView.Enabled)
			{
				const uint32_t row = i / columnCount;
				const uint32_t column = i % columnCount;
				const float normalizedX = ((static_cast<float>(column) + jitterDistribution(rng)) / static_cast<float>(columnCount)) * 2.0f - 1.0f;
				const float normalizedY = ((static_cast<float>(row) + jitterDistribution(rng)) / static_cast<float>(rowCount)) * 2.0f - 1.0f;
				const float depthLerp = (static_cast<float>(i % rowCount) + depthDistribution(rng)) / static_cast<float>(rowCount);
				const float depth = m_SpawnView.NearDistance + (m_SpawnView.FarDistance - m_SpawnView.NearDistance) * depthLerp;
				const float viewX = normalizedX * tanHalfFovX * depth * 0.88f;
				const float viewY = normalizedY * tanHalfFovY * depth * 0.82f;
				spawn.Position = AddScaled(m_SpawnView.Eye, m_SpawnView.Forward, m_SpawnView.Right, m_SpawnView.Up, depth, viewX, viewY);
			}
			else
			{
				std::uniform_real_distribution<float> positionDistribution(-30.0f, 30.0f);
				std::uniform_real_distribution<float> heightDistribution(-8.0f, 8.0f);
				spawn.Position = {
					positionDistribution(rng),
					heightDistribution(rng),
					positionDistribution(rng)
				};
			}
			spawn.Velocity = {
				velocityDistribution(rng),
				velocityDistribution(rng) * 0.25f,
				velocityDistribution(rng)
			};
			spawn.Scale = { scale, scale, scale };
			spawn.Tint = {
				colorDistribution(rng),
				colorDistribution(rng),
				colorDistribution(rng),
				spiderType ? 0.95f : 1.0f
			};
			spawn.AngularSpeed = spinDistribution(rng);
			spawn.SpinAngle = angleDistribution(rng);
			spawn.LocalBoundsMin = spiderType
				? DirectX::XMFLOAT3{ -24.0f, -8.0f, -24.0f }
				: DirectX::XMFLOAT3{ -0.5f, -0.5f, -0.5f };
			spawn.LocalBoundsMax = spiderType
				? DirectX::XMFLOAT3{ 24.0f, 8.0f, 24.0f }
				: DirectX::XMFLOAT3{ 0.5f, 0.5f, 0.5f };
			spawnData.push_back(spawn);
		}

		return spawnData;
	}

	uint32_t BenchmarkRunner::GetCpuMaterializedObjectCount(const BenchmarkConfig& config) const noexcept
	{
		return (std::min)(config.ObjectCount, MaxCpuMaterializedObjects());
	}

	BenchmarkStats BenchmarkRunner::RunScenario(const BenchmarkConfig& config) const
	{
		const BenchmarkConfig sanitizedConfig = SanitizeConfig(config);
		const std::vector<BenchmarkSpawnData> spawnData = CreateSpawnData(sanitizedConfig);
		std::vector<BenchmarkRenderInstance> renderInstances;

		NonEcsObjectStore nonEcsStore;
		EcsObjectStore ecsStore;
		if (sanitizedConfig.Mode == BenchmarkMode::NonEcs)
		{
			nonEcsStore.Build(spawnData, sanitizedConfig.ObjectType);
		}
		else
		{
			ecsStore.Build(spawnData, sanitizedConfig.ObjectType);
		}

		constexpr float fixedDeltaTime = 1.0f / 60.0f;
		for (uint32_t i = 0; i < sanitizedConfig.WarmupFrames; ++i)
		{
			if (sanitizedConfig.Mode == BenchmarkMode::NonEcs)
			{
				(void)TickStore(nonEcsStore, renderInstances, fixedDeltaTime);
			}
			else
			{
				(void)TickStore(ecsStore, renderInstances, fixedDeltaTime);
			}
		}

		BenchmarkStats accumulatedStats;
		for (uint32_t i = 0; i < sanitizedConfig.SampleFrames; ++i)
		{
			const BenchmarkStats sampleStats = sanitizedConfig.Mode == BenchmarkMode::NonEcs
				? TickStore(nonEcsStore, renderInstances, fixedDeltaTime)
				: TickStore(ecsStore, renderInstances, fixedDeltaTime);
			Accumulate(accumulatedStats, sampleStats);
		}
		Average(accumulatedStats, sanitizedConfig.SampleFrames);
		return accumulatedStats;
	}

	void BenchmarkRunner::RebuildStores()
	{
		m_SpawnData = CreateSpawnData(m_Config);
		m_NonEcsStore.Build(m_SpawnData, m_Config.ObjectType);
		m_EcsStore.Build(m_SpawnData, m_Config.ObjectType);
		m_RenderInstances.clear();
		m_RenderInstances.reserve(GetCpuMaterializedObjectCount());
		m_LastStats = {};
		m_IsConfigured = true;
	}

	void BenchmarkRunner::LogScenarioResult(const BenchmarkScenarioResult& result) const
	{
		std::ostringstream stream;
		stream << std::fixed << std::setprecision(3)
			<< "[ECS Benchmark] "
			<< "mode=" << ToString(result.Config.Mode)
			<< " type=" << ToString(result.Config.ObjectType)
			<< " count=" << result.Config.ObjectCount
			<< " update_ms=" << result.Stats.UpdateMs
			<< " render_collect_ms=" << result.Stats.RenderSubmitMs
			<< " cpu_frame_ms=" << result.Stats.FrameMs
			<< " cpu_fps=" << std::setprecision(1) << result.Stats.Fps
			<< " visible=" << result.Stats.VisibleObjectCount
			<< "\n";

		const std::string message = stream.str();
		OutputDebugStringA(message.c_str());
	}
}
