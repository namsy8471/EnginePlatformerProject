// main.cpp : 애플리케이션에 대한 진입점을 정의합니다.

#include "EngineCore/GameApp.h"

// 나중에는 별도 파일(Engine.h/cpp)로 분리될 테스트용 클래스
class TestEngine : public GameApp
{
public:
    TestEngine(HINSTANCE hInstance) : GameApp(hInstance) {}
    ~TestEngine() {}

    bool Init() override
    {
        if (!GameApp::Init()) return false;
        // TODO: 여기서 DX12 초기화
        return true;
    }

    void OnResize() override
    {
        // TODO: 스왑체인 리사이즈
    }

    void Update(float dt) override
    {
        // TODO: 게임 로직
    }

    void Render() override
    {
        // TODO: DX12 그리기 명령
    }
};

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

    // 디버그 모드에서 메모리 누수 감지 켜기
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    TestEngine theApp(hInstance);

    if (!theApp.Init())
        return 0;

    return theApp.Run();
}



