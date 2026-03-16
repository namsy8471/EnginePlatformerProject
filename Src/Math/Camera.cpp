#include "Camera.h"
#include "Input/InputSystem.h"
#include <algorithm>
#include <cmath>

Camera::Camera()
{
	XMStoreFloat4x4(&m_View, XMMatrixIdentity());
	XMStoreFloat4x4(&m_Projection, XMMatrixIdentity());
	UpdateViewMatrix();
	SetLens(m_FovY, m_Aspect, m_NearZ, m_FarZ);
}

void Camera::SetPosition(float x, float y, float z) noexcept
{
	m_Transform.Translation = { x, y, z };
	m_ViewDirty = true;
}

void Camera::SetPosition(const XMFLOAT3& position) noexcept
{
	m_Transform.Translation = position;
	m_ViewDirty = true;
}

void Camera::SetLens(float fovY, float aspect, float nearZ, float farZ) noexcept
{
	m_FovY = fovY;
	m_Aspect = aspect;
	m_NearZ = nearZ;
	m_FarZ = farZ;

	Math::Store(m_Projection, XMMatrixPerspectiveFovLH(fovY, aspect, nearZ, farZ));
}

void Camera::SetRotation(float pitch, float yaw) noexcept
{
	m_Pitch = std::clamp(pitch, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
	m_Yaw = yaw;
	m_Transform.Rotation = Math::Transform::FromEuler(Math::ZeroVector3(), m_Pitch, m_Yaw, 0.0f).Rotation;
	m_ViewDirty = true;
}

void Camera::LookAt(const XMFLOAT3& eye, const XMFLOAT3& target, const XMFLOAT3& up) noexcept
{
	const XMVECTOR pos = Math::Load(eye);
	const XMVECTOR targetPos = Math::Load(target);
	const XMVECTOR upVec = Math::Load(up);

	const XMVECTOR forward = XMVector3Normalize(targetPos - pos);
	const XMVECTOR right = XMVector3Normalize(XMVector3Cross(upVec, forward));
	const XMVECTOR cameraUp = XMVector3Cross(forward, right);

	m_Transform.Translation = eye;
	DirectX::XMFLOAT3 forwardFloat = {};
	Math::Store(forwardFloat, forward);
	m_Pitch = std::clamp(std::asinf(forwardFloat.y), -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
	m_Yaw = std::atan2f(forwardFloat.x, forwardFloat.z);
	m_Transform.Rotation = Math::Transform::FromEuler(Math::ZeroVector3(), m_Pitch, m_Yaw, 0.0f).Rotation;
	Math::Store(m_Forward, forward);
	Math::Store(m_Right, right);
	Math::Store(m_Up, cameraUp);

	m_ViewDirty = true;
	UpdateViewMatrix();
}

void Camera::Walk(float distance) noexcept
{
	const XMVECTOR forward = Math::Load(m_Forward);
	const XMVECTOR position = Math::Load(m_Transform.Translation);
	const XMVECTOR newPosition = position + forward * distance;
	Math::Store(m_Transform.Translation, newPosition);
	m_ViewDirty = true;
}

void Camera::Strafe(float distance) noexcept
{
	const XMVECTOR right = Math::Load(m_Right);
	const XMVECTOR position = Math::Load(m_Transform.Translation);
	const XMVECTOR newPosition = position + right * distance;
	Math::Store(m_Transform.Translation, newPosition);
	m_ViewDirty = true;
}

void Camera::Rise(float distance) noexcept
{
	const XMVECTOR up = Math::Load(m_Up);
	const XMVECTOR position = Math::Load(m_Transform.Translation);
	const XMVECTOR newPosition = position + up * distance;
	Math::Store(m_Transform.Translation, newPosition);
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
		Pitch(input.GetMouseDeltaY() * m_MouseSensitivity);
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
	const Math::Transform rotationTransform = Math::Transform::FromEuler(Math::ZeroVector3(), m_Pitch, m_Yaw, 0.0f);
	m_Transform.Rotation = rotationTransform.Rotation;
	const XMMATRIX rotation = rotationTransform.ToXmMatrix();

	const XMVECTOR defaultForward = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
	const XMVECTOR defaultRight = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
	const XMVECTOR defaultUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	const XMVECTOR forward = XMVector3TransformNormal(defaultForward, rotation);
	const XMVECTOR right = XMVector3TransformNormal(defaultRight, rotation);
	const XMVECTOR up = XMVector3TransformNormal(defaultUp, rotation);
	const XMVECTOR position = Math::Load(m_Transform.Translation);
	const XMVECTOR target = position + forward;

	Math::Store(m_Forward, forward);
	Math::Store(m_Right, right);
	Math::Store(m_Up, up);

	Math::Store(m_View, XMMatrixLookAtLH(position, target, up));
	m_ViewDirty = false;
}

XMMATRIX Camera::GetViewMatrix() const noexcept
{
	return Math::Load(m_View);
}

XMMATRIX Camera::GetProjectionMatrix() const noexcept
{
	return Math::Load(m_Projection);
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
	return Math::ToFloat4x4(GetViewProjectionMatrix());
}
