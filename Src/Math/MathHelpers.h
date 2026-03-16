#pragma once

#include <DirectXMath.h>

namespace Math
{
	[[nodiscard]] inline DirectX::XMVECTOR Load(const DirectX::XMFLOAT3& value) noexcept
	{
		return DirectX::XMLoadFloat3(&value);
	}

	[[nodiscard]] inline DirectX::XMVECTOR Load(const DirectX::XMFLOAT4& value) noexcept
	{
		return DirectX::XMLoadFloat4(&value);
	}

	[[nodiscard]] inline DirectX::XMMATRIX Load(const DirectX::XMFLOAT4X4& value) noexcept
	{
		return DirectX::XMLoadFloat4x4(&value);
	}

	inline void Store(DirectX::XMFLOAT3& destination, DirectX::FXMVECTOR value) noexcept
	{
		DirectX::XMStoreFloat3(&destination, value);
	}

	inline void Store(DirectX::XMFLOAT4& destination, DirectX::FXMVECTOR value) noexcept
	{
		DirectX::XMStoreFloat4(&destination, value);
	}

	inline void Store(DirectX::XMFLOAT4X4& destination, DirectX::FXMMATRIX value) noexcept
	{
		DirectX::XMStoreFloat4x4(&destination, value);
	}

	[[nodiscard]] inline DirectX::XMFLOAT4X4 ToFloat4x4(DirectX::FXMMATRIX value) noexcept
	{
		DirectX::XMFLOAT4X4 result = {};
		Store(result, value);
		return result;
	}

	[[nodiscard]] inline DirectX::XMFLOAT3 Add(const DirectX::XMFLOAT3& lhs, const DirectX::XMFLOAT3& rhs) noexcept
	{
		DirectX::XMFLOAT3 result = {};
		Store(result, DirectX::XMVectorAdd(Load(lhs), Load(rhs)));
		return result;
	}

	[[nodiscard]] inline DirectX::XMFLOAT3 Multiply(const DirectX::XMFLOAT3& lhs, const DirectX::XMFLOAT3& rhs) noexcept
	{
		DirectX::XMFLOAT3 result = {};
		Store(result, DirectX::XMVectorMultiply(Load(lhs), Load(rhs)));
		return result;
	}

	[[nodiscard]] inline DirectX::XMFLOAT4 MultiplyQuaternion(const DirectX::XMFLOAT4& lhs, const DirectX::XMFLOAT4& rhs) noexcept
	{
		DirectX::XMFLOAT4 result = {};
		Store(result, DirectX::XMQuaternionMultiply(Load(lhs), Load(rhs)));
		return result;
	}

	[[nodiscard]] inline DirectX::XMFLOAT4 IdentityQuaternion() noexcept
	{
		return { 0.0f, 0.0f, 0.0f, 1.0f };
	}

	[[nodiscard]] inline DirectX::XMFLOAT3 OneVector3() noexcept
	{
		return { 1.0f, 1.0f, 1.0f };
	}

	[[nodiscard]] inline DirectX::XMFLOAT3 ZeroVector3() noexcept
	{
		return { 0.0f, 0.0f, 0.0f };
	}

	[[nodiscard]] inline DirectX::XMMATRIX MakeTranslation(const DirectX::XMFLOAT3& translation) noexcept
	{
		return DirectX::XMMatrixTranslation(translation.x, translation.y, translation.z);
	}

	[[nodiscard]] inline DirectX::XMMATRIX MakeRotationQuaternion(const DirectX::XMFLOAT4& rotation) noexcept
	{
		return DirectX::XMMatrixRotationQuaternion(Load(rotation));
	}

	[[nodiscard]] inline DirectX::XMMATRIX MakeScale(const DirectX::XMFLOAT3& scale) noexcept
	{
		return DirectX::XMMatrixScaling(scale.x, scale.y, scale.z);
	}

	[[nodiscard]] inline DirectX::XMMATRIX ComposeTRS(const DirectX::XMFLOAT3& translation, const DirectX::XMFLOAT4& rotation, const DirectX::XMFLOAT3& scale) noexcept
	{
		return MakeScale(scale) * MakeRotationQuaternion(rotation) * MakeTranslation(translation);
	}

	[[nodiscard]] inline bool DecomposeMatrix(const DirectX::XMFLOAT4X4& matrix, DirectX::XMFLOAT3& translation, DirectX::XMFLOAT4& rotation, DirectX::XMFLOAT3& scale) noexcept
	{
		DirectX::XMVECTOR scaleVector = {};
		DirectX::XMVECTOR rotationQuaternion = {};
		DirectX::XMVECTOR translationVector = {};
		if (!DirectX::XMMatrixDecompose(&scaleVector, &rotationQuaternion, &translationVector, Load(matrix)))
		{
			translation = ZeroVector3();
			rotation = IdentityQuaternion();
			scale = OneVector3();
			return false;
		}

		Store(translation, translationVector);
		Store(rotation, rotationQuaternion);
		Store(scale, scaleVector);
		return true;
	}

	[[nodiscard]] inline DirectX::XMFLOAT3 TransformPoint(const DirectX::XMFLOAT3& point, const DirectX::XMFLOAT4X4& matrix) noexcept
	{
		DirectX::XMFLOAT3 result = {};
		Store(result, DirectX::XMVector3TransformCoord(Load(point), Load(matrix)));
		return result;
	}

	[[nodiscard]] inline DirectX::XMFLOAT3 TransformVector(const DirectX::XMFLOAT3& vector, const DirectX::XMFLOAT4X4& matrix) noexcept
	{
		DirectX::XMFLOAT3 result = {};
		Store(result, DirectX::XMVector3TransformNormal(Load(vector), Load(matrix)));
		return result;
	}
}
