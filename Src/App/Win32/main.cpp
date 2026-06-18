// main.cpp : 애플리케이션에 대한 진입점을 정의합니다.

#include "Core/Engine/Engine.h"
#include "Memory/MemorySystem.h"

#include <shellapi.h>

#include <cstdint>
#include <fstream>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

#if defined(DEBUG) || defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace
{
	void AppendSmokeProcessLog(const EngineStartupOptions& options, std::string_view message)
	{
		if (!options.SmokeLogPath)
		{
			return;
		}

		std::error_code errorCode;
		const std::filesystem::path parentPath = options.SmokeLogPath->parent_path();
		if (!parentPath.empty())
		{
			std::filesystem::create_directories(parentPath, errorCode);
		}

		std::ofstream smokeLog(*options.SmokeLogPath, std::ios::app);
		if (smokeLog)
		{
			smokeLog << message << '\n';
		}
	}

	[[nodiscard]] std::optional<std::uint32_t> ParsePositiveFrameCount(std::wstring_view text)
	{
		if (text.empty())
		{
			return std::nullopt;
		}

		std::uint32_t value = 0;
		for (const wchar_t character : text)
		{
			if (character < L'0' || character > L'9')
			{
				return std::nullopt;
			}

			value = value * 10u + static_cast<std::uint32_t>(character - L'0');
			if (value > 10000u)
			{
				value = 10000u;
				break;
			}
		}

		return value > 0 ? std::optional<std::uint32_t>(value) : std::nullopt;
	}

	[[nodiscard]] std::optional<GraphicsAPI> ParseGraphicsApi(std::wstring_view text)
	{
		if (text == L"dx12" || text == L"directx12" || text == L"DirectX12")
		{
			return GraphicsAPI::DirectX12;
		}
		if (text == L"vulkan" || text == L"Vulkan")
		{
			return GraphicsAPI::Vulkan;
		}
		return std::nullopt;
	}

	[[nodiscard]] std::optional<RenderMode> ParseRenderMode(std::wstring_view text)
	{
		if (text == L"forward" || text == L"Forward")
		{
			return RenderMode::Forward;
		}
		if (text == L"deferred" || text == L"Deferred")
		{
			return RenderMode::Deferred;
		}
		if (text == L"forward+" || text == L"Forward+")
		{
			return RenderMode::ForwardPlus;
		}
		return std::nullopt;
	}

	[[nodiscard]] EngineStartupOptions ParseStartupOptions()
	{
		EngineStartupOptions options;
		int argumentCount = 0;
		LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
		if (!arguments)
		{
			return options;
		}

		for (int argumentIndex = 1; argumentIndex < argumentCount; ++argumentIndex)
		{
			const std::wstring_view argument(arguments[argumentIndex]);
			constexpr std::wstring_view projectPrefix = L"--project=";
			constexpr std::wstring_view runtimePackagePrefix = L"--runtime-package=";
			constexpr std::wstring_view smokeTestPrefix = L"--smoke-test=";
			constexpr std::wstring_view smokeFramesPrefix = L"--smoke-frames=";
			constexpr std::wstring_view smokeLogPrefix = L"--smoke-log=";
			constexpr std::wstring_view smokeApiPrefix = L"--smoke-api=";
			constexpr std::wstring_view smokeRenderModePrefix = L"--smoke-render-mode=";
			if (argument == L"--project" && argumentIndex + 1 < argumentCount)
			{
				options.ProjectFilePath = std::filesystem::path(arguments[++argumentIndex]);
				continue;
			}

			if (argument.starts_with(projectPrefix))
			{
				options.ProjectFilePath = std::filesystem::path(argument.substr(projectPrefix.size()));
				continue;
			}

			if (argument == L"--runtime-package" && argumentIndex + 1 < argumentCount)
			{
				options.RuntimePackageManifestPath = std::filesystem::path(arguments[++argumentIndex]);
				continue;
			}

			if (argument.starts_with(runtimePackagePrefix))
			{
				options.RuntimePackageManifestPath = std::filesystem::path(argument.substr(runtimePackagePrefix.size()));
				continue;
			}

			if (argument == L"--smoke-test")
			{
				options.SmokeTestFrameLimit = 3u;
				continue;
			}

			if (argument.starts_with(smokeTestPrefix))
			{
				options.SmokeTestFrameLimit = ParsePositiveFrameCount(argument.substr(smokeTestPrefix.size())).value_or(3u);
				continue;
			}

			if (argument == L"--smoke-frames" && argumentIndex + 1 < argumentCount)
			{
				const std::wstring_view frameText(arguments[++argumentIndex]);
				options.SmokeTestFrameLimit = ParsePositiveFrameCount(frameText).value_or(3u);
				continue;
			}

			if (argument.starts_with(smokeFramesPrefix))
			{
				options.SmokeTestFrameLimit = ParsePositiveFrameCount(argument.substr(smokeFramesPrefix.size())).value_or(3u);
				continue;
			}

			if (argument == L"--smoke-log" && argumentIndex + 1 < argumentCount)
			{
				options.SmokeLogPath = std::filesystem::path(arguments[++argumentIndex]);
				continue;
			}

			if (argument.starts_with(smokeLogPrefix))
			{
				options.SmokeLogPath = std::filesystem::path(argument.substr(smokeLogPrefix.size()));
				continue;
			}

			if (argument == L"--smoke-api" && argumentIndex + 1 < argumentCount)
			{
				const std::wstring_view apiText(arguments[++argumentIndex]);
				options.SmokeGraphicsApi = ParseGraphicsApi(apiText);
				continue;
			}

			if (argument.starts_with(smokeApiPrefix))
			{
				options.SmokeGraphicsApi = ParseGraphicsApi(argument.substr(smokeApiPrefix.size()));
				continue;
			}

			if (argument == L"--smoke-render-mode" && argumentIndex + 1 < argumentCount)
			{
				const std::wstring_view renderModeText(arguments[++argumentIndex]);
				options.SmokeRenderMode = ParseRenderMode(renderModeText);
				continue;
			}

			if (argument.starts_with(smokeRenderModePrefix))
			{
				options.SmokeRenderMode = ParseRenderMode(argument.substr(smokeRenderModePrefix.size()));
			}
		}

		LocalFree(arguments);
		return options;
	}
}

int APIENTRY wWinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR lpCmdLine,
	_In_ int nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	// 디버그 모드에서 메모리 누수 감지 활성화
#if defined(DEBUG) || defined(_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

	Memory::Initialize();

	int result = 0;
	const EngineStartupOptions startupOptions = ParseStartupOptions();
	{
		Engine engine(hInstance, startupOptions);

		if (!engine.Init())
		{
			result = 0;
		}
		else
		{
			result = engine.Run();
		}
	}

	AppendSmokeProcessLog(startupOptions, "Smoke shutdown: engine object destroyed.");
	Memory::Shutdown();
	AppendSmokeProcessLog(startupOptions, "Smoke shutdown: memory system stopped.");
	return result;
}









