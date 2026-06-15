#pragma once

#include <DirectXMath.h>

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace Physics
{
	enum class RigidBodyType : uint8_t
	{
		Static,
		Dynamic,
		Kinematic
	};

	enum class ColliderShape : uint8_t
	{
		Box,
		Sphere,
		Capsule,
		Plane
	};

	struct RigidBodyComponent
	{
		RigidBodyType Type = RigidBodyType::Dynamic;
		float Mass = 1.0f;
		float LinearDamping = 0.05f;
		float AngularDamping = 0.05f;
		bool UseGravity = true;
		DirectX::XMFLOAT3 LinearVelocity = { 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT3 AngularVelocity = { 0.0f, 0.0f, 0.0f };
	};

	struct ColliderComponent
	{
		ColliderShape Shape = ColliderShape::Box;
		DirectX::XMFLOAT3 Size = { 1.0f, 1.0f, 1.0f };
		float Radius = 0.5f;
		float Height = 1.0f;
		DirectX::XMFLOAT3 Offset = { 0.0f, 0.0f, 0.0f };
		bool IsTrigger = false;
	};

	struct PhysicsMaterialComponent
	{
		float StaticFriction = 0.5f;
		float DynamicFriction = 0.5f;
		float Restitution = 0.05f;
	};

	[[nodiscard]] constexpr std::string_view ToString(RigidBodyType type) noexcept
	{
		switch (type)
		{
		case RigidBodyType::Static:
			return "Static";
		case RigidBodyType::Dynamic:
			return "Dynamic";
		case RigidBodyType::Kinematic:
			return "Kinematic";
		default:
			return "Dynamic";
		}
	}

	[[nodiscard]] constexpr std::string_view ToString(ColliderShape shape) noexcept
	{
		switch (shape)
		{
		case ColliderShape::Box:
			return "Box";
		case ColliderShape::Sphere:
			return "Sphere";
		case ColliderShape::Capsule:
			return "Capsule";
		case ColliderShape::Plane:
			return "Plane";
		default:
			return "Box";
		}
	}

	[[nodiscard]] inline RigidBodyType RigidBodyTypeFromString(std::string_view text) noexcept
	{
		if (text == "Static")
		{
			return RigidBodyType::Static;
		}
		if (text == "Kinematic")
		{
			return RigidBodyType::Kinematic;
		}
		return RigidBodyType::Dynamic;
	}

	[[nodiscard]] inline ColliderShape ColliderShapeFromString(std::string_view text) noexcept
	{
		if (text == "Sphere")
		{
			return ColliderShape::Sphere;
		}
		if (text == "Capsule")
		{
			return ColliderShape::Capsule;
		}
		if (text == "Plane")
		{
			return ColliderShape::Plane;
		}
		return ColliderShape::Box;
	}

	[[nodiscard]] inline uint32_t ToIndex(RigidBodyType type) noexcept
	{
		return std::min<uint32_t>(static_cast<uint32_t>(type), 2u);
	}

	[[nodiscard]] inline uint32_t ToIndex(ColliderShape shape) noexcept
	{
		return std::min<uint32_t>(static_cast<uint32_t>(shape), 3u);
	}
}
