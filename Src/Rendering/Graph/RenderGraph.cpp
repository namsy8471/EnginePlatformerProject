#include "Rendering/Graph/RenderGraph.h"

#include "Rendering/RHI/GraphicsCommon.h"
#include "Rendering/RenderMode.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Rendering
{
	const char* ToString(RenderPassKind kind) noexcept
	{
		switch (kind)
		{
		case RenderPassKind::Setup:
			return "Setup";
		case RenderPassKind::Clear:
			return "Clear";
		case RenderPassKind::World:
			return "World";
		case RenderPassKind::Shadow:
			return "Shadow";
		case RenderPassKind::Geometry:
			return "Geometry";
		case RenderPassKind::Lighting:
			return "Lighting";
		case RenderPassKind::Transparency:
			return "Transparency";
		case RenderPassKind::PostProcess:
			return "PostProcess";
		case RenderPassKind::Editor:
			return "Editor";
		case RenderPassKind::Present:
			return "Present";
		case RenderPassKind::Debug:
		default:
			return "Debug";
		}
	}

	void RenderGraph::BeginFrame(uint64_t frameIndex, GraphicsAPI api, RenderMode mode)
	{
		m_Api = api;
		m_Mode = mode;
		m_Passes.clear();
		m_Stats = RenderGraphStats{
			.FrameIndex = frameIndex,
			.UsesDeferred = mode == RenderMode::Deferred,
			.UsesHdr = mode == RenderMode::Deferred
		};
		m_FrameOpen = true;
	}

	size_t RenderGraph::AddPass(RenderGraphPass pass)
	{
		if (!m_FrameOpen)
		{
			return (std::numeric_limits<size_t>::max)();
		}
		m_Passes.push_back(std::move(pass));
		return m_Passes.size() - 1;
	}

	size_t RenderGraph::AddPass(std::string_view name, RenderPassKind kind, std::string_view target, std::string_view notes, bool enabled)
	{
		return AddPass(RenderGraphPass{
			.Name = std::string(name),
			.Kind = kind,
			.Target = std::string(target),
			.Notes = std::string(notes),
			.Enabled = enabled
			});
	}

	void RenderGraph::SetPassCpuTime(size_t passIndex, double cpuMilliseconds, bool includeInStats)
	{
		if (passIndex >= m_Passes.size())
		{
			return;
		}

		RenderGraphPass& pass = m_Passes[passIndex];
		pass.CpuMilliseconds = (std::max)(0.0, cpuMilliseconds);
		pass.HasCpuTiming = true;
		pass.IncludeCpuInStats = includeInStats;
	}

	void RenderGraph::EndFrame()
	{
		m_Stats.PassCount = m_Passes.size();
		m_Stats.EnabledPassCount = 0;
		m_Stats.WorldPassCount = 0;
		m_Stats.ShadowPassCount = 0;
		m_Stats.GeometryPassCount = 0;
		m_Stats.LightingPassCount = 0;
		m_Stats.PostProcessPassCount = 0;
		m_Stats.EditorPassCount = 0;
		m_Stats.TimedPassCount = 0;
		m_Stats.TotalCpuMs = 0.0;
		m_Stats.SetupCpuMs = 0.0;
		m_Stats.ClearCpuMs = 0.0;
		m_Stats.WorldCpuMs = 0.0;
		m_Stats.ShadowCpuMs = 0.0;
		m_Stats.GeometryCpuMs = 0.0;
		m_Stats.LightingCpuMs = 0.0;
		m_Stats.TransparencyCpuMs = 0.0;
		m_Stats.PostProcessCpuMs = 0.0;
		m_Stats.EditorCpuMs = 0.0;
		m_Stats.PresentCpuMs = 0.0;
		m_Stats.DebugCpuMs = 0.0;

		for (const RenderGraphPass& pass : m_Passes)
		{
			if (!pass.Enabled)
			{
				continue;
			}
			++m_Stats.EnabledPassCount;
			if (pass.HasCpuTiming)
			{
				++m_Stats.TimedPassCount;
				if (pass.IncludeCpuInStats)
				{
					m_Stats.TotalCpuMs += pass.CpuMilliseconds;
					switch (pass.Kind)
					{
					case RenderPassKind::Setup:
						m_Stats.SetupCpuMs += pass.CpuMilliseconds;
						break;
					case RenderPassKind::Clear:
						m_Stats.ClearCpuMs += pass.CpuMilliseconds;
						break;
					case RenderPassKind::World:
						m_Stats.WorldCpuMs += pass.CpuMilliseconds;
						break;
					case RenderPassKind::Shadow:
						m_Stats.ShadowCpuMs += pass.CpuMilliseconds;
						break;
					case RenderPassKind::Geometry:
						m_Stats.GeometryCpuMs += pass.CpuMilliseconds;
						break;
					case RenderPassKind::Lighting:
						m_Stats.LightingCpuMs += pass.CpuMilliseconds;
						break;
					case RenderPassKind::Transparency:
						m_Stats.TransparencyCpuMs += pass.CpuMilliseconds;
						break;
					case RenderPassKind::PostProcess:
						m_Stats.PostProcessCpuMs += pass.CpuMilliseconds;
						break;
					case RenderPassKind::Editor:
						m_Stats.EditorCpuMs += pass.CpuMilliseconds;
						break;
					case RenderPassKind::Present:
						m_Stats.PresentCpuMs += pass.CpuMilliseconds;
						break;
					case RenderPassKind::Debug:
					default:
						m_Stats.DebugCpuMs += pass.CpuMilliseconds;
						break;
					}
				}
			}

			switch (pass.Kind)
			{
			case RenderPassKind::World:
				++m_Stats.WorldPassCount;
				break;
			case RenderPassKind::Shadow:
				++m_Stats.ShadowPassCount;
				break;
			case RenderPassKind::Geometry:
				++m_Stats.GeometryPassCount;
				break;
			case RenderPassKind::Lighting:
				++m_Stats.LightingPassCount;
				break;
			case RenderPassKind::PostProcess:
				++m_Stats.PostProcessPassCount;
				break;
			case RenderPassKind::Editor:
				++m_Stats.EditorPassCount;
				break;
			default:
				break;
			}
		}
		m_FrameOpen = false;
		m_LastCompletedPasses = m_Passes;
		m_LastCompletedStats = m_Stats;
	}

	void RenderGraph::Clear()
	{
		m_Passes.clear();
		m_LastCompletedPasses.clear();
		m_Stats = {};
		m_LastCompletedStats = {};
		m_FrameOpen = false;
	}

	RenderGraphStats RenderGraph::GetStats() const
	{
		return m_FrameOpen ? m_LastCompletedStats : m_Stats;
	}

	const std::vector<RenderGraphPass>& RenderGraph::GetPasses() const noexcept
	{
		return m_FrameOpen ? m_LastCompletedPasses : m_Passes;
	}
}
