// main.cpp : 애플리케이션에 대한 진입점을 정의합니다.

#include "EngineCore/Engine.h"

#if defined(DEBUG) || defined(_DEBUG)
#include <crtdbg.h>
#endif

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

	Engine engine(hInstance);

	if (!engine.Init())
	{
		return 0;
	}

	return engine.Run();
}









