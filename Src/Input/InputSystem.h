#pragma once

#include <windows.h>
#include <array>
#include <cstdint>

// 키보드와 마우스 입력 상태를 관리하는 싱글톤 클래스
// Win32 메시지를 처리하여 현재 프레임의 입력 상태를 추적합니다.
class InputSystem
{
public:
	static InputSystem& Get();

	// Win32 메시지 처리 (GameApp의 MsgProc에서 호출)
	void ProcessMessage(UINT message, WPARAM wParam, LPARAM lParam);

	// 프레임 시작 시 호출하여 이전 프레임 상태 갱신
	void Update();

	// 키보드 상태 쿼리
	[[nodiscard]] bool IsKeyDown(int vkCode) const noexcept;
	[[nodiscard]] bool IsKeyUp(int vkCode) const noexcept;
	[[nodiscard]] bool IsKeyPressed(int vkCode) const noexcept;  // 이번 프레임에 눌림
	[[nodiscard]] bool IsKeyReleased(int vkCode) const noexcept; // 이번 프레임에 떼어짐

	// 마우스 상태 쿼리
	[[nodiscard]] bool IsMouseButtonDown(int button) const noexcept; // 0=Left, 1=Right, 2=Middle
	[[nodiscard]] bool IsMouseButtonPressed(int button) const noexcept;
	[[nodiscard]] bool IsMouseButtonReleased(int button) const noexcept;

	// 마우스 위치 및 이동량
	[[nodiscard]] int GetMouseX() const noexcept { return m_MouseX; }
	[[nodiscard]] int GetMouseY() const noexcept { return m_MouseY; }
	[[nodiscard]] int GetMouseDeltaX() const noexcept { return m_MouseDeltaX; }
	[[nodiscard]] int GetMouseDeltaY() const noexcept { return m_MouseDeltaY; }
	[[nodiscard]] int GetMouseWheelDelta() const noexcept { return m_MouseWheelDelta; }

	// 마우스 커서 표시/숨김
	void ShowCursor(bool show);
	void LockCursor(HWND hwnd, bool lock);
	void ResetFrameState();

private:
	InputSystem() = default;
	~InputSystem() = default;

	InputSystem(const InputSystem&) = delete;
	InputSystem& operator=(const InputSystem&) = delete;

	// 256개의 가상 키 코드 지원
	static constexpr size_t kKeyCount = 256;
	static constexpr size_t kMouseButtonCount = 3;

	std::array<bool, kKeyCount> m_KeyState = {};          // 현재 프레임 키 상태
	std::array<bool, kKeyCount> m_PrevKeyState = {};      // 이전 프레임 키 상태

	std::array<bool, kMouseButtonCount> m_MouseButtonState = {};
	std::array<bool, kMouseButtonCount> m_PrevMouseButtonState = {};

	int m_MouseX = 0;
	int m_MouseY = 0;
	int m_PrevMouseX = 0;
	int m_PrevMouseY = 0;
	int m_MouseDeltaX = 0;
	int m_MouseDeltaY = 0;
	int m_MouseWheelDelta = 0;

	bool m_CursorLocked = false;
	HWND m_LockedWindow = nullptr;
	bool m_IsCursorVisible = true;
};
