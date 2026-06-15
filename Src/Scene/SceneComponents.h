#pragma once

#include "Assets/StaticMesh.h"
#include "Scene/TransformComponent.h"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct CpuMaterialTexture
{
	std::filesystem::path Path;
	std::vector<unsigned char> Pixels = { 255, 255, 255, 255 };
	int Width = 1;
	int Height = 1;
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
};
