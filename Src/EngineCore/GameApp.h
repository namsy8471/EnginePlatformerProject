#pragma once
#include <windows.h>
#include <string>

class GameApp
{
public:
	GameApp(HINSTANCE hInstance);
	virtual ~GameApp();

	int Run();

	virtual bool Init();

	virtual LRESULT MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

protected:
	virtual void Update(float deltaTime) = 0;
	virtual void Render() = 0;
	virtual void OnResize() = 0;

	bool InitMainWindow();

protected:
	HINSTANCE m_hAppInst = nullptr;	// 애플리케이션 인스턴스 핸들
	HWND      m_hMainWnd = nullptr;	// 메인 윈도우 핸들

	bool      m_AppPaused = false;	// 애플리케이션 일시 정지 플래그
	bool      m_Minimized = false;	// 애플리케이션 최소화 플래그
	bool      m_Maximized = false;	// 애플리케이션 최대화 플래그
	bool      m_Resizing = false;	// 사용자가 크기 조절 중인지 여부

	std::wstring mMainWndCaption = L"Game Application";
	int m_ClientWidth = 1280;
	int m_ClientHeight = 720;
};

