#pragma once

#include "Assets/StaticMesh.h"
#include "Physics/PhysicsComponents.h"
#include "Scene/SceneTypes.h"
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

struct EditorStateComponent
{
	bool VisibleInScene = true;
	bool PickableInScene = true;
};

struct SceneHierarchyComponent
{
	EntityId Parent = InvalidEntityId;
	bool Expanded = true;
};

struct MeshComponent
{
	std::unique_ptr<Asset::StaticMeshAsset> Asset;
	std::vector<CpuMaterialTexture> MaterialTextures;

	MeshComponent() = default;

	MeshComponent(const MeshComponent& other)
		: Asset(other.Asset ? std::make_unique<Asset::StaticMeshAsset>(*other.Asset) : nullptr)
		, MaterialTextures(other.MaterialTextures)
	{
	}

	MeshComponent& operator=(const MeshComponent& other)
	{
		if (this == &other)
		{
			return *this;
		}

		Asset = other.Asset ? std::make_unique<Asset::StaticMeshAsset>(*other.Asset) : nullptr;
		MaterialTextures = other.MaterialTextures;
		return *this;
	}

	MeshComponent(MeshComponent&&) noexcept = default;
	MeshComponent& operator=(MeshComponent&&) noexcept = default;
};

struct AnimatorComponent
{
	uint32_t CurrentClipIndex = 0;
	float TimeSeconds = 0.0f;
	float Speed = 1.0f;
	bool Playing = true;
	bool Loop = true;
};

struct PrefabInstanceComponent
{
	std::filesystem::path PrefabPath;
	std::string SourceName;
	bool TrackPrefabOverrides = true;
};

struct SceneReferenceComponent
{
	std::filesystem::path ScenePath;
	bool LoadAdditively = true;
	bool AutoLoad = false;
};

enum class ScriptLanguage : uint8_t
{
	Native,
	Lua,
	CSharpLike,
	GDScriptLike
};

struct ScriptComponent
{
	std::filesystem::path ScriptPath;
	std::string ClassName = "GameScript";
	ScriptLanguage Language = ScriptLanguage::Native;
	bool RunInEditor = false;
};

struct Sprite2DComponent
{
	std::filesystem::path TexturePath;
	DirectX::XMFLOAT4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };
	DirectX::XMFLOAT2 Size = { 1.0f, 1.0f };
	DirectX::XMFLOAT2 Pivot = { 0.5f, 0.5f };
	int SortingLayer = 0;
	int OrderInLayer = 0;
};

enum class UiElementKind : uint8_t
{
	Panel,
	Label,
	Button,
	Image
};

struct UiElementComponent
{
	UiElementKind Kind = UiElementKind::Panel;
	std::string Text = "UI Element";
	DirectX::XMFLOAT2 AnchorMin = { 0.0f, 0.0f };
	DirectX::XMFLOAT2 AnchorMax = { 0.0f, 0.0f };
	DirectX::XMFLOAT2 Position = { 0.0f, 0.0f };
	DirectX::XMFLOAT2 Size = { 160.0f, 48.0f };
	DirectX::XMFLOAT4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };
};

struct AudioSourceComponent
{
	std::filesystem::path ClipPath;
	float Volume = 1.0f;
	float Pitch = 1.0f;
	bool Loop = false;
	bool PlayOnStart = false;
	bool Spatialize = true;
	float MinDistance = 1.0f;
	float MaxDistance = 50.0f;
};

struct NavigationAgentComponent
{
	float Radius = 0.35f;
	float Height = 1.8f;
	float Speed = 3.5f;
	float Acceleration = 12.0f;
	DirectX::XMFLOAT3 Target = { 0.0f, 0.0f, 0.0f };
	bool HasTarget = false;
};

struct NetworkIdentityComponent
{
	uint64_t NetworkId = 0;
	std::string PrefabKey;
	bool ReplicateTransform = true;
	bool ServerAuthoritative = true;
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
	PhysicsMaterial,
	PrefabInstance,
	SceneReference,
	Script,
	Sprite2D,
	UiElement,
	AudioSource,
	NavigationAgent,
	NetworkIdentity
};
