#pragma once

#include <DirectXMath.h>
#include <windows.h>

using namespace DirectX;

// 3D 카메라 클래스
// View/Projection 행렬을 관리하고 입력에 따라 이동/회전합니다.
class Camera
{
public:
	Camera();
	~Camera() = default;

	// 카메라 속성 설정
	void SetPosition(float x, float y, float z) noexcept;
	void SetPosition(const XMFLOAT3& position) noexcept;
	void SetLens(float fovY, float aspect, float nearZ, float farZ) noexcept;

	// 카메라 방향 제어
	void SetRotation(float pitch, float yaw) noexcept;
	void LookAt(const XMFLOAT3& eye, const XMFLOAT3& target, const XMFLOAT3& up) noexcept;

	// 카메라 이동
	void Walk(float distance) noexcept;    // 전후 이동
	void Strafe(float distance) noexcept;  // 좌우 이동
	void Rise(float distance) noexcept;    // 상하 이동

	// 카메라 회전
	void Pitch(float angle) noexcept;      // X축 회전
	void Yaw(float angle) noexcept;        // Y축 회전

	// 입력 기반 업데이트
	void Update(float deltaTime, HWND hwnd);

	// 행렬 업데이트
	void UpdateViewMatrix() noexcept;

	// Getter
	[[nodiscard]] XMFLOAT3 GetPosition() const noexcept { return m_Position; }
	[[nodiscard]] XMFLOAT3 GetForward() const noexcept { return m_Forward; }
	[[nodiscard]] XMFLOAT3 GetRight() const noexcept { return m_Right; }
	[[nodiscard]] XMFLOAT3 GetUp() const noexcept { return m_Up; }

	[[nodiscard]] XMMATRIX GetViewMatrix() const noexcept;
	[[nodiscard]] XMMATRIX GetProjectionMatrix() const noexcept;
	[[nodiscard]] XMMATRIX GetViewProjectionMatrix() const noexcept;

	[[nodiscard]] XMFLOAT4X4 GetViewMatrix4x4() const noexcept;
	[[nodiscard]] XMFLOAT4X4 GetProjectionMatrix4x4() const noexcept;
	[[nodiscard]] XMFLOAT4X4 GetViewProjectionMatrix4x4() const noexcept;

private:
	// 카메라 위치 및 방향
	XMFLOAT3 m_Position = { 0.0f, 0.0f, -5.0f };
	XMFLOAT3 m_Forward = { 0.0f, 0.0f, 1.0f };
	XMFLOAT3 m_Right = { 1.0f, 0.0f, 0.0f };
	XMFLOAT3 m_Up = { 0.0f, 1.0f, 0.0f };

	// 회전값 (라디안)
	float m_Pitch = 0.0f;
	float m_Yaw = 0.0f;

	// 렌즈 속성
	float m_FovY = XM_PIDIV4;
	float m_Aspect = 16.0f / 9.0f;
	float m_NearZ = 0.1f;
	float m_FarZ = 1000.0f;

	// 이동 속도
	float m_MoveSpeed = 400.0f;
	float m_MouseSensitivity = 0.002f;

	// 행렬 캐시
	XMFLOAT4X4 m_View = {};
	XMFLOAT4X4 m_Projection = {};
	bool m_ViewDirty = true;
};
