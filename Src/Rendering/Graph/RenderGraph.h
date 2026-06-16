#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class GraphicsAPI : std::uint8_t;
enum class RenderMode;

namespace Rendering
{
	enum class RenderPassKind : uint8_t
	{
		Setup,
		Clear,
		World,
		Shadow,
		Geometry,
		Lighting,
		Transparency,
		PostProcess,
		Editor,
		Present,
		Debug
	};

	struct RenderGraphPass
	{
		std::string Name;
		RenderPassKind Kind = RenderPassKind::Debug;
		std::string Target;
		std::string Notes;
		double CpuMilliseconds = 0.0;
		bool Enabled = true;
		bool HasCpuTiming = false;
		bool IncludeCpuInStats = true;
	};

	struct RenderGraphStats
	{
		uint64_t FrameIndex = 0;
		size_t PassCount = 0;
		size_t EnabledPassCount = 0;
		size_t WorldPassCount = 0;
		size_t ShadowPassCount = 0;
		size_t GeometryPassCount = 0;
		size_t LightingPassCount = 0;
		size_t PostProcessPassCount = 0;
		size_t EditorPassCount = 0;
		size_t TimedPassCount = 0;
		double TotalCpuMs = 0.0;
		double SetupCpuMs = 0.0;
		double ClearCpuMs = 0.0;
		double WorldCpuMs = 0.0;
		double ShadowCpuMs = 0.0;
		double GeometryCpuMs = 0.0;
		double LightingCpuMs = 0.0;
		double TransparencyCpuMs = 0.0;
		double PostProcessCpuMs = 0.0;
		double EditorCpuMs = 0.0;
		double PresentCpuMs = 0.0;
		double DebugCpuMs = 0.0;
		bool UsesDeferred = false;
		bool UsesHdr = false;
	};

	class RenderGraph
	{
	public:
		void BeginFrame(uint64_t frameIndex, GraphicsAPI api, RenderMode mode);
		size_t AddPass(RenderGraphPass pass);
		size_t AddPass(std::string_view name, RenderPassKind kind, std::string_view target = {}, std::string_view notes = {}, bool enabled = true);
		void SetPassCpuTime(size_t passIndex, double cpuMilliseconds, bool includeInStats = true);
		void EndFrame();
		void Clear();

		[[nodiscard]] RenderGraphStats GetStats() const;
		[[nodiscard]] const std::vector<RenderGraphPass>& GetPasses() const noexcept;

	private:
		std::vector<RenderGraphPass> m_Passes;
		std::vector<RenderGraphPass> m_LastCompletedPasses;
		RenderGraphStats m_Stats;
		RenderGraphStats m_LastCompletedStats;
		GraphicsAPI m_Api{};
		RenderMode m_Mode{};
		bool m_FrameOpen = false;
	};

	[[nodiscard]] const char* ToString(RenderPassKind kind) noexcept;
}
