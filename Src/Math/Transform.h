#pragma once

#include "MathHelpers.h"

namespace Math
{
	struct Transform
	{
		DirectX::XMFLOAT3 Translation = ZeroVector3();
		DirectX::XMFLOAT4 Rotation = IdentityQuaternion();
		DirectX::XMFLOAT3 Scale = OneVector3();

		Transform() = default;

		Transform(const DirectX::XMFLOAT3& translation, const DirectX::XMFLOAT4& rotation, const DirectX::XMFLOAT3& scale) noexcept
			: Translation(translation)
			, Rotation(rotation)
			, Scale(scale)
		{
		}

		[[nodiscard]] DirectX::XMMATRIX ToXmMatrix() const noexcept
		{
			return ComposeTRS(Translation, Rotation, Scale);
		}

		[[nodiscard]] DirectX::XMFLOAT4X4 ToMatrix() const noexcept
		{
			DirectX::XMFLOAT4X4 result = {};
			Store(result, ToXmMatrix());
			return result;
		}

		[[nodiscard]] static Transform Identity() noexcept
		{
			return Transform();
		}

		[[nodiscard]] static Transform FromMatrix(const DirectX::XMFLOAT4X4& matrix) noexcept
		{
			DirectX::XMFLOAT3 translation = ZeroVector3();
			DirectX::XMFLOAT4 rotation = IdentityQuaternion();
			DirectX::XMFLOAT3 scale = OneVector3();
			DecomposeMatrix(matrix, translation, rotation, scale);
			return Transform(translation, rotation, scale);
		}

		[[nodiscard]] static Transform FromEuler(const DirectX::XMFLOAT3& translation, float pitch, float yaw, float roll, const DirectX::XMFLOAT3& scale = OneVector3()) noexcept
		{
			DirectX::XMFLOAT4 rotation = {};
			Store(rotation, DirectX::XMQuaternionRotationRollPitchYaw(pitch, yaw, roll));
			return Transform(translation, rotation, scale);
		}

		[[nodiscard]] Transform operator*(const Transform& rhs) const noexcept
		{
			return FromMatrix(ToFloat4x4(ToXmMatrix() * rhs.ToXmMatrix()));
		}

		Transform& operator*=(const Transform& rhs) noexcept
		{
			*this = *this * rhs;
			return *this;
		}
	};

	[[nodiscard]] inline Transform Combine(const Transform& lhs, const Transform& rhs) noexcept
	{
		return lhs * rhs;
	}
}
