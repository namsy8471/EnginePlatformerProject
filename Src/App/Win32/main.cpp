// main.cpp : 애플리케이션에 대한 진입점을 정의합니다.

#include "Core/Engine/Engine.h"

#include <shellapi.h>

#include <filesystem>
#include <string_view>

#if defined(DEBUG) || defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace
{
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
			if (argument == L"--project" && argumentIndex + 1 < argumentCount)
			{
				options.ProjectFilePath = std::filesystem::path(arguments[++argumentIndex]);
				continue;
			}

			if (argument.starts_with(projectPrefix))
			{
				options.ProjectFilePath = std::filesystem::path(argument.substr(projectPrefix.size()));
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

	Engine engine(hInstance, ParseStartupOptions());

	if (!engine.Init())
	{
		return 0;
	}

	return engine.Run();
}









