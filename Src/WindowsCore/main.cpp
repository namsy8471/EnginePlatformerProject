// main.cpp : 애플리케이션에 대한 진입점을 정의합니다.

#include "EngineCore/GameApp.h"

#include "RHI/IGraphicsDevice.h"
#include "RHI/ICommandList.h"
#include "RHI/IBuffer.h"        // 나중에 쓸 테니 미리 넣어둬도 됨
#include "RHI/GraphicsCommon.h" // GraphicsAPI Enum 때문에 필요

// 나중에는 별도 파일(Engine.h/cpp)로 분리될 테스트용 클래스
class TestEngine : public GameApp
{
	IGraphicsDevice* m_Device = nullptr;
	ICommandList* m_CmdList = nullptr;

public:
    TestEngine(HINSTANCE hInstance) : GameApp(hInstance) {}
    ~TestEngine() {}

    bool Init() override
    {
        if (!GameApp::Init()) return false;
        // TODO: 여기서 DX12 초기화
        
		m_Device = IGraphicsDevice::Create(
            GraphicsAPI::DirectX12,
            m_hMainWnd,
            m_ClientWidth,
            m_ClientHeight);

        if (!m_Device->Init())
            return false;

		m_CmdList = m_Device->CreateCommandList();

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
		// 커맨드 리스트 리셋
        m_CmdList->Reset();

		// 뷰포트 시저포트 설정
		m_CmdList->SetViewport(0, 0, static_cast<float>(m_ClientWidth), static_cast<float>(m_ClientHeight));
		m_CmdList->SetScissorRect(0, 0, m_ClientWidth, m_ClientHeight);

		// 백버퍼를 렌더타겟 상태로 전환
		IGpuResource* backBuffer = m_Device->GetBackBufferResource();
        m_CmdList->ResourceBarrier(
            backBuffer,
            ResourceState::Present,
			ResourceState::RenderTarget);

		// 렌더타겟 및 뎁스스텐실 뷰 설정
		void* rtvHandle = m_Device->GetCurrentBackBufferRTV();
		void* dsvHandle = m_Device->GetDepthStencilView();

		m_CmdList->SetRenderTargets(rtvHandle, dsvHandle);

		// 화면 클리어
		float clearColor[4] = { 0.0f, 0.2f, 0.4f, 1.0f };
		m_CmdList->ClearRenderTarget(rtvHandle, clearColor);
		m_CmdList->ClearDepthStencil(dsvHandle, 1.0, 0);

		// TODO: 실제 그리기 명령 추가
        m_CmdList->ResourceBarrier(
            backBuffer,
            ResourceState::RenderTarget,
			ResourceState::Present);

		m_CmdList->Close();

		// 커맨드 리스트 실행
		m_Device->ExecuteCommandList(m_CmdList);

		// 화면 출력
		m_Device->Present();

		// 다음 프레임으로 이동
		m_Device->MoveToNextFrame();
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



