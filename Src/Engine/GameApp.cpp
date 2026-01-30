#include "GameApp.h"
#include "framework.h"
#include "resource.h"
#include <cassert>

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CREATE)
    {
        CREATESTRUCT* pCreateStruct = reinterpret_cast<CREATESTRUCT*>(lParam);
        GameApp* pApp = reinterpret_cast<GameApp*>(pCreateStruct->lpCreateParams);
        assert(pApp != nullptr);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pApp));
    }

	GameApp* pApp = reinterpret_cast<GameApp*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    if(pApp) return pApp->MsgProc(hWnd, msg, wParam, lParam);

	return DefWindowProc(hWnd, msg, wParam, lParam);
}

// 정보 대화 상자의 메시지 처리기입니다.
INT_PTR CALLBACK About(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (msg)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

GameApp::GameApp(HINSTANCE hInstance) : m_hAppInst(hInstance)
{
}

GameApp::~GameApp()
{
}

bool GameApp::Init()
{
	if (!InitMainWindow())
        return false;
    
    return true;
}

bool GameApp::InitMainWindow()
{
	// Register the window class
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = m_hAppInst;
    wcex.hIcon = LoadIcon(m_hAppInst, MAKEINTRESOURCE(IDI_DX12ENINGE));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_DX12ENINGE);
    wcex.lpszClassName = L"EngineWndClass";
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    if (!RegisterClassExW(&wcex)) {
		MessageBoxW(nullptr, L"Failed to register Window Class\n윈도우 클래스 등록에 실패했습니다.", L"ERROR", MB_OK);
        return false;
    }

	RECT r = { 0, 0, m_ClientWidth, m_ClientHeight };
    
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    
	// Create the window
    m_hMainWnd = CreateWindowW(
        L"EngineWndClass",
        mMainWndCaption.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        r.right - r.left,
        r.bottom - r.top,
        nullptr,
        nullptr,
        m_hAppInst,
        this);

    if (!m_hMainWnd) {
        MessageBoxW(nullptr, L"Failed to create Main Window\n메인 윈도우 생성에 실패했습니다.", L"ERROR", MB_OK);
        return false;
    }

    ShowWindow(m_hMainWnd, SW_SHOW);
    UpdateWindow(m_hMainWnd);

	return true;
}

int GameApp::Run()
{
	MSG msg = { 0 };
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            if (!m_AppPaused)
            {
                // TODO: TImer.Tick 추가
                Update(0.0f); // TODO: deltaTime 전달
                Render();

            }
            else
            {
                Sleep(100);
            }
        }
    }

	return static_cast<int>(msg.wParam);
}

LRESULT GameApp::MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
        {
            m_AppPaused = true;
        }
        else
        {
            m_AppPaused = false;
        }
        return 0;

    case WM_ENTERSIZEMOVE:
		m_AppPaused = true;
        m_Resizing = true;
		return 0;

	case WM_EXITSIZEMOVE:
        m_AppPaused = false;
        m_Resizing = false;
		OnResize();
        return 0;

    case WM_SIZE:
    {
        m_ClientWidth = LOWORD(lParam);
        m_ClientHeight = HIWORD(lParam);
        if (wParam == SIZE_MINIMIZED)
        {
            m_AppPaused = true;
            m_Minimized = true;
            m_Maximized = false;
        }
        else if (wParam == SIZE_MAXIMIZED)
        {
            m_AppPaused = false;
            m_Minimized = false;
            m_Maximized = true;
            OnResize();
        }
        else if (wParam == SIZE_RESTORED)
        {
            // 리사이징 중이 아닐 때만 즉시 처리
            if (!m_Resizing && !m_Minimized)
            {
                m_AppPaused = false;
                OnResize();
            }
        }
        return 0;
	}

    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);
        // 메뉴 선택을 구문 분석합니다:
        switch (wmId)
        {
        case IDM_ABOUT:
            DialogBox(m_hAppInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        default:
            return DefWindowProc(hWnd, msg, wParam, lParam);
        }
    }
    break;
    
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
}


