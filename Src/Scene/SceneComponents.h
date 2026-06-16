#pragma once

#include "Assets/StaticMesh.h"
#include "Physics/PhysicsComponents.h"
#include "Scene/TransformComponent.h"

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct CpuMaterialTextureSlot
{
	std::filesystem::path Path;
	std::vector<unsigned char> Pixels = { 255, 255, 255, 255 };
	int Width = 1;
	int Height = 1;
	bool Srgb = true;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return Width > 0 && Height > 0 && !Pixels.empty();
	}
};

struct CpuMaterialTexture
{
	std::array<CpuMaterialTextureSlot, Asset::kMaterialTextureSlotCount> Slots = {};

	[[nodiscard]] CpuMaterialTextureSlot& Slot(Asset::MaterialTextureSlot slot) noexcept
	{
		return Slots[Asset::MaterialTextureSlotIndex(slot)];
	}

	[[nodiscard]] const CpuMaterialTextureSlot& Slot(Asset::MaterialTextureSlot slot) const noexcept
	{
		return Slots[Asset::MaterialTextureSlotIndex(slot)];
	}

	[[nodiscard]] const std::filesystem::path& BaseColorPath() const noexcept
	{
		return Slot(Asset::MaterialTextureSlot::BaseColor).Path;
	}
};

struct NameComponent
{
	std::string Name;
};

struct MeshComponent
{
	std::unique_ptr<Asset::StaticMeshAsset> Asset;
	std::vector<CpuMaterialTexture> MaterialTextures;
};

struct AnimatorComponent
{
	uint32_t CurrentClipIndex = 0;
	float TimeSeconds = 0.0f;
	float Speed = 1.0f;
	bool Playing = true;
	bool Loop = true;
};

struct BoundsComponent
{
	DirectX::XMFLOAT3 LocalMin = { 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 LocalMax = { 0.0f, 0.0f, 0.0f };
};

struct CameraComponent
{
	float FovY = DirectX::XM_PIDIV4;
	float NearZ = 0.1f;
	float FarZ = 1000.0f;
	bool IsGameCamera = false;
};

enum class LightType : uint8_t
{
	Directional,
	Point,
	Spot
};

struct LightComponent
{
	LightType Type = LightType::Directional;
	DirectX::XMFLOAT3 Color = { 1.0f, 0.96f, 0.86f };
	float Intensity = 2.5f;
	float Range = 200.0f;
	float SpotAngle = DirectX::XM_PIDIV4;
	bool Enabled = true;
	bool CastShadows = true;
	float ShadowBias = 0.0015f;
	float ShadowNormalBias = 0.02f;
	float ShadowStrength = 0.75f;
};

using RigidBodyComponent = Physics::RigidBodyComponent;
using ColliderComponent = Physics::ColliderComponent;
using PhysicsMaterialComponent = Physics::PhysicsMaterialComponent;

enum class SceneComponentKind : uint8_t
{
	Mesh,
	Animator,
	Camera,
	Light,
	RigidBody,
	Collider,
	PhysicsMaterial
};
