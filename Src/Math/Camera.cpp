#include "Camera.h"
#include "Input/InputSystem.h"
#include <algorithm>

Camera::Camera()
{
	XMStoreFloat4x4(&m_View, XMMatrixIdentity());
	XMStoreFloat4x4(&m_Projection, XMMatrixIdentity());
	UpdateViewMatrix();
	SetLens(m_FovY, m_Aspect, m_NearZ, m_FarZ);
}

void Camera::SetPosition(float x, float y, float z) noexcept
{
	m_Position = { x, y, z };
	m_ViewDirty = true;
}

void Camera::SetPosition(const XMFLOAT3& position) noexcept
{
	m_Position = position;
	m_ViewDirty = true;
}

void Camera::SetLens(float fovY, float aspect, float nearZ, float farZ) noexcept
{
	m_FovY = fovY;
	m_Aspect = aspect;
	m_NearZ = nearZ;
	m_FarZ = farZ;

	const XMMATRIX projection = XMMatrixPerspectiveFovLH(fovY, aspect, nearZ, farZ);
	XMStoreFloat4x4(&m_Projection, projection);
}

void Camera::SetRotation(float pitch, float yaw) noexcept
{
	m_Pitch = std::clamp(pitch, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
	m_Yaw = yaw;
	m_ViewDirty = true;
}

void Camera::LookAt(const XMFLOAT3& eye, const XMFLOAT3& target, const XMFLOAT3& up) noexcept
{
	const XMVECTOR pos = XMLoadFloat3(&eye);
	const XMVECTOR targetPos = XMLoadFloat3(&target);
	const XMVECTOR upVec = XMLoadFloat3(&up);

	const XMVECTOR forward = XMVector3Normalize(targetPos - pos);
	const XMVECTOR right = XMVector3Normalize(XMVector3Cross(upVec, forward));
	const XMVECTOR cameraUp = XMVector3Cross(forward, right);

	XMStoreFloat3(&m_Position, pos);
	XMStoreFloat3(&m_Forward, forward);
	XMStoreFloat3(&m_Right, right);
	XMStoreFloat3(&m_Up, cameraUp);

	m_ViewDirty = true;
	UpdateViewMatrix();
}

void Camera::Walk(float distance) noexcept
{
	const XMVECTOR forward = XMLoadFloat3(&m_Forward);
	const XMVECTOR position = XMLoadFloat3(&m_Position);
	const XMVECTOR newPosition = position + forward * distance;
	XMStoreFloat3(&m_Position, newPosition);
	m_ViewDirty = true;
}

void Camera::Strafe(float distance) noexcept
{
	const XMVECTOR right = XMLoadFloat3(&m_Right);
	const XMVECTOR position = XMLoadFloat3(&m_Position);
	const XMVECTOR newPosition = position + right * distance;
	XMStoreFloat3(&m_Position, newPosition);
	m_ViewDirty = true;
}

void Camera::Rise(float distance) noexcept
{
	const XMVECTOR up = XMLoadFloat3(&m_Up);
	const XMVECTOR position = XMLoadFloat3(&m_Position);
	const XMVECTOR newPosition = position + up * distance;
	XMStoreFloat3(&m_Position, newPosition);
	m_ViewDirty = true;
}

void Camera::Pitch(float angle) noexcept
{
	m_Pitch = std::clamp(m_Pitch + angle, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
	m_ViewDirty = true;
}

void Camera::Yaw(float angle) noexcept
{
	m_Yaw += angle;
	m_ViewDirty = true;
}

void Camera::Update(float deltaTime, HWND hwnd)
{
	InputSystem& input = InputSystem::Get();
	const float moveAmount = m_MoveSpeed * deltaTime;

	// WASD 이동
	if (input.IsKeyDown('W')) Walk(moveAmount);
	if (input.IsKeyDown('S')) Walk(-moveAmount);
	if (input.IsKeyDown('A')) Strafe(-moveAmount);
	if (input.IsKeyDown('D')) Strafe(moveAmount);
	if (input.IsKeyDown('Q')) Rise(-moveAmount);
	if (input.IsKeyDown('E')) Rise(moveAmount);

	// 우클릭으로 마우스 룩
	if (input.IsMouseButtonDown(1))
	{
		input.LockCursor(hwnd, true);
		Yaw(input.GetMouseDeltaX() * m_MouseSensitivity);
		Pitch(-input.GetMouseDeltaY() * m_MouseSensitivity);
	}
	else
	{
		input.LockCursor(hwnd, false);
	}

	if (m_ViewDirty)
	{
		UpdateViewMatrix();
	}
}

void Camera::UpdateViewMatrix() noexcept
{
	const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(m_Pitch, m_Yaw, 0.0f);

	const XMVECTOR defaultForward = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
	const XMVECTOR defaultRight = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
	const XMVECTOR defaultUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	const XMVECTOR forward = XMVector3TransformNormal(defaultForward, rotation);
	const XMVECTOR right = XMVector3TransformNormal(defaultRight, rotation);
	const XMVECTOR up = XMVector3TransformNormal(defaultUp, rotation);
	const XMVECTOR position = XMLoadFloat3(&m_Position);
	const XMVECTOR target = position + forward;

	XMStoreFloat3(&m_Forward, forward);
	XMStoreFloat3(&m_Right, right);
	XMStoreFloat3(&m_Up, up);

	const XMMATRIX view = XMMatrixLookAtLH(position, target, up);
	XMStoreFloat4x4(&m_View, view);
	m_ViewDirty = false;
}

XMMATRIX Camera::GetViewMatrix() const noexcept
{
	return XMLoadFloat4x4(&m_View);
}

XMMATRIX Camera::GetProjectionMatrix() const noexcept
{
	return XMLoadFloat4x4(&m_Projection);
}

XMMATRIX Camera::GetViewProjectionMatrix() const noexcept
{
	return GetViewMatrix() * GetProjectionMatrix();
}

XMFLOAT4X4 Camera::GetViewMatrix4x4() const noexcept
{
	return m_View;
}

XMFLOAT4X4 Camera::GetProjectionMatrix4x4() const noexcept
{
	return m_Projection;
}

XMFLOAT4X4 Camera::GetViewProjectionMatrix4x4() const noexcept
{
	XMFLOAT4X4 result;
	XMStoreFloat4x4(&result, GetViewProjectionMatrix());
	return result;
}
