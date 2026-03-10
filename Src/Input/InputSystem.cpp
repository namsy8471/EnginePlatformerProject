#include "InputSystem.h"
#include <algorithm>
#include <windowsx.h>

InputSystem& InputSystem::Get()
{
	static InputSystem instance;
	return instance;
}

void InputSystem::ProcessMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (wParam < kKeyCount)
		{
			m_KeyState[wParam] = true;
		}
		break;

	case WM_KEYUP:
	case WM_SYSKEYUP:
		if (wParam < kKeyCount)
		{
			m_KeyState[wParam] = false;
		}
		break;

	case WM_LBUTTONDOWN:
		SetCapture(reinterpret_cast<HWND>(GetActiveWindow()));
		m_MouseButtonState[0] = true;
		break;

	case WM_LBUTTONUP:
		ReleaseCapture();
		m_MouseButtonState[0] = false;
		break;

	case WM_RBUTTONDOWN:
		SetCapture(reinterpret_cast<HWND>(GetActiveWindow()));
		m_MouseButtonState[1] = true;
		break;

	case WM_RBUTTONUP:
		ReleaseCapture();
		m_MouseButtonState[1] = false;
		break;

	case WM_MBUTTONDOWN:
		SetCapture(reinterpret_cast<HWND>(GetActiveWindow()));
		m_MouseButtonState[2] = true;
		break;

	case WM_MBUTTONUP:
		ReleaseCapture();
		m_MouseButtonState[2] = false;
		break;

	case WM_MOUSEMOVE:
	{
		const int newMouseX = GET_X_LPARAM(lParam);
		const int newMouseY = GET_Y_LPARAM(lParam);
		m_MouseDeltaX += newMouseX - m_MouseX;
		m_MouseDeltaY += newMouseY - m_MouseY;
		m_MouseX = newMouseX;
		m_MouseY = newMouseY;
		break;
	}

	case WM_MOUSEWHEEL:
		m_MouseWheelDelta += GET_WHEEL_DELTA_WPARAM(wParam);
		break;
	}
}

void InputSystem::Update()
{
	// 입력 조회가 끝난 뒤 호출되어 이번 프레임의 상태를 다음 프레임의 이전 상태로 넘깁니다.
	m_PrevKeyState = m_KeyState;
	m_PrevMouseButtonState = m_MouseButtonState;
	ResetFrameState();

	if (m_CursorLocked && m_LockedWindow)
	{
		RECT rect = {};
		GetClientRect(m_LockedWindow, &rect);
		const int centerX = (rect.right - rect.left) / 2;
		const int centerY = (rect.bottom - rect.top) / 2;

		POINT point = { centerX, centerY };
		ClientToScreen(m_LockedWindow, &point);
		SetCursorPos(point.x, point.y);

		m_MouseX = centerX;
		m_MouseY = centerY;
	}
}

void InputSystem::ResetFrameState()
{
	m_MouseDeltaX = 0;
	m_MouseDeltaY = 0;
	m_MouseWheelDelta = 0;
}

bool InputSystem::IsKeyDown(int vkCode) const noexcept
{
	if (vkCode < 0 || vkCode >= static_cast<int>(kKeyCount))
	{
		return false;
	}
	return m_KeyState[vkCode];
}

bool InputSystem::IsKeyUp(int vkCode) const noexcept
{
	return !IsKeyDown(vkCode);
}

bool InputSystem::IsKeyPressed(int vkCode) const noexcept
{
	if (vkCode < 0 || vkCode >= static_cast<int>(kKeyCount))
	{
		return false;
	}
	return m_KeyState[vkCode] && !m_PrevKeyState[vkCode];
}

bool InputSystem::IsKeyReleased(int vkCode) const noexcept
{
	if (vkCode < 0 || vkCode >= static_cast<int>(kKeyCount))
	{
		return false;
	}
	return !m_KeyState[vkCode] && m_PrevKeyState[vkCode];
}

bool InputSystem::IsMouseButtonDown(int button) const noexcept
{
	if (button < 0 || button >= static_cast<int>(kMouseButtonCount))
	{
		return false;
	}
	return m_MouseButtonState[button];
}

bool InputSystem::IsMouseButtonPressed(int button) const noexcept
{
	if (button < 0 || button >= static_cast<int>(kMouseButtonCount))
	{
		return false;
	}
	return m_MouseButtonState[button] && !m_PrevMouseButtonState[button];
}

bool InputSystem::IsMouseButtonReleased(int button) const noexcept
{
	if (button < 0 || button >= static_cast<int>(kMouseButtonCount))
	{
		return false;
	}
	return !m_MouseButtonState[button] && m_PrevMouseButtonState[button];
}

void InputSystem::ShowCursor(bool show)
{
	if (m_IsCursorVisible == show)
	{
		return;
	}

	::ShowCursor(show ? TRUE : FALSE);
	m_IsCursorVisible = show;
}

void InputSystem::LockCursor(HWND hwnd, bool lock)
{
	m_CursorLocked = lock;
	m_LockedWindow = lock ? hwnd : nullptr;

	if (lock)
	{
		ShowCursor(false);
	}
	else
	{
		ShowCursor(true);
	}
}
