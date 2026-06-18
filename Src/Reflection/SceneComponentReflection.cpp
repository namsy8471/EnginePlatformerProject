#include "SceneComponentReflection.h"

#include <array>
#include <ranges>

namespace Reflection
{
	namespace
	{
		using enum PropertyValueKind;

		constexpr std::array kMeshProperties = {
			PropertyDescriptor{ "Asset", Path },
			PropertyDescriptor{ "PrimitiveKind", Enum },
			PropertyDescriptor{ "VertexCount", Int },
			PropertyDescriptor{ "IndexCount", Int },
			PropertyDescriptor{ "SubmeshCount", Int },
			PropertyDescriptor{ "MaterialCount", Int },
			PropertyDescriptor{ "AnimationCount", Int },
			PropertyDescriptor{ "Materials", String }
		};

		constexpr std::array kAnimatorProperties = {
			PropertyDescriptor{ "CurrentClipIndex", Int },
			PropertyDescriptor{ "TimeSeconds", Float },
			PropertyDescriptor{ "Speed", Float },
			PropertyDescriptor{ "Playing", Bool },
			PropertyDescriptor{ "Loop", Bool }
		};

		constexpr std::array kCameraProperties = {
			PropertyDescriptor{ "FovY", Float },
			PropertyDescriptor{ "NearZ", Float },
			PropertyDescriptor{ "FarZ", Float },
			PropertyDescriptor{ "IsGameCamera", Bool }
		};

		constexpr std::array kLightProperties = {
			PropertyDescriptor{ "Type", Enum },
			PropertyDescriptor{ "Color", Float3 },
			PropertyDescriptor{ "Intensity", Float },
			PropertyDescriptor{ "Range", Float },
			PropertyDescriptor{ "SpotAngle", Float },
			PropertyDescriptor{ "Enabled", Bool },
			PropertyDescriptor{ "CastShadows", Bool }
		};

		constexpr std::array kRigidBodyProperties = {
			PropertyDescriptor{ "Type", Enum },
			PropertyDescriptor{ "Mass", Float },
			PropertyDescriptor{ "UseGravity", Bool },
			PropertyDescriptor{ "LinearVelocity", Float3 },
			PropertyDescriptor{ "AngularVelocity", Float3 }
		};

		constexpr std::array kColliderProperties = {
			PropertyDescriptor{ "Shape", Enum },
			PropertyDescriptor{ "Size", Float3 },
			PropertyDescriptor{ "Radius", Float },
			PropertyDescriptor{ "Height", Float },
			PropertyDescriptor{ "Offset", Float3 },
			PropertyDescriptor{ "IsTrigger", Bool }
		};

		constexpr std::array kPhysicsMaterialProperties = {
			PropertyDescriptor{ "StaticFriction", Float },
			PropertyDescriptor{ "DynamicFriction", Float },
			PropertyDescriptor{ "Restitution", Float }
		};

		constexpr std::array kPrefabProperties = {
			PropertyDescriptor{ "PrefabPath", Path },
			PropertyDescriptor{ "SourceName", String },
			PropertyDescriptor{ "TrackPrefabOverrides", Bool }
		};

		constexpr std::array kSceneReferenceProperties = {
			PropertyDescriptor{ "ScenePath", Path },
			PropertyDescriptor{ "LoadAdditively", Bool },
			PropertyDescriptor{ "AutoLoad", Bool }
		};

		constexpr std::array kScriptProperties = {
			PropertyDescriptor{ "ScriptPath", Path },
			PropertyDescriptor{ "ClassName", String },
			PropertyDescriptor{ "Language", Enum },
			PropertyDescriptor{ "RunInEditor", Bool }
		};

		constexpr std::array kSpriteProperties = {
			PropertyDescriptor{ "TexturePath", Path },
			PropertyDescriptor{ "Color", Float4 },
			PropertyDescriptor{ "Size", Float2 },
			PropertyDescriptor{ "Pivot", Float2 },
			PropertyDescriptor{ "SortingLayer", Int },
			PropertyDescriptor{ "OrderInLayer", Int }
		};

		constexpr std::array kUiProperties = {
			PropertyDescriptor{ "Kind", Enum },
			PropertyDescriptor{ "Text", String },
			PropertyDescriptor{ "AnchorMin", Float2 },
			PropertyDescriptor{ "AnchorMax", Float2 },
			PropertyDescriptor{ "Position", Float2 },
			PropertyDescriptor{ "Size", Float2 },
			PropertyDescriptor{ "Color", Float4 }
		};

		constexpr std::array kAudioProperties = {
			PropertyDescriptor{ "ClipPath", Path },
			PropertyDescriptor{ "Volume", Float },
			PropertyDescriptor{ "Pitch", Float },
			PropertyDescriptor{ "Loop", Bool },
			PropertyDescriptor{ "PlayOnStart", Bool },
			PropertyDescriptor{ "Spatialize", Bool }
		};

		constexpr std::array kNavigationProperties = {
			PropertyDescriptor{ "Radius", Float },
			PropertyDescriptor{ "Height", Float },
			PropertyDescriptor{ "Speed", Float },
			PropertyDescriptor{ "Acceleration", Float },
			PropertyDescriptor{ "Target", Float3 },
			PropertyDescriptor{ "HasTarget", Bool }
		};

		constexpr std::array kNetworkProperties = {
			PropertyDescriptor{ "NetworkId", Uint64 },
			PropertyDescriptor{ "PrefabKey", String },
			PropertyDescriptor{ "ReplicateTransform", Bool },
			PropertyDescriptor{ "ServerAuthoritative", Bool }
		};

		constexpr std::array kComponentDescriptors = {
			ComponentDescriptor{ SceneComponentKind::Mesh, "Mesh", true, true, true, kMeshProperties },
			ComponentDescriptor{ SceneComponentKind::Animator, "Animator", true, true, true, kAnimatorProperties },
			ComponentDescriptor{ SceneComponentKind::Camera, "Camera", true, true, true, kCameraProperties },
			ComponentDescriptor{ SceneComponentKind::Light, "Light", true, true, true, kLightProperties },
			ComponentDescriptor{ SceneComponentKind::RigidBody, "Rigidbody", true, true, true, kRigidBodyProperties },
			ComponentDescriptor{ SceneComponentKind::Collider, "Collider", true, true, true, kColliderProperties },
			ComponentDescriptor{ SceneComponentKind::PhysicsMaterial, "Physics Material", true, true, true, kPhysicsMaterialProperties },
			ComponentDescriptor{ SceneComponentKind::PrefabInstance, "Prefab Instance", true, true, true, kPrefabProperties },
			ComponentDescriptor{ SceneComponentKind::SceneReference, "Scene Reference", true, true, true, kSceneReferenceProperties },
			ComponentDescriptor{ SceneComponentKind::Script, "Script", true, true, true, kScriptProperties },
			ComponentDescriptor{ SceneComponentKind::Sprite2D, "Sprite 2D", true, true, true, kSpriteProperties },
			ComponentDescriptor{ SceneComponentKind::UiElement, "UI Element", true, true, true, kUiProperties },
			ComponentDescriptor{ SceneComponentKind::AudioSource, "Audio Source", true, true, true, kAudioProperties },
			ComponentDescriptor{ SceneComponentKind::NavigationAgent, "Navigation Agent", true, true, true, kNavigationProperties },
			ComponentDescriptor{ SceneComponentKind::NetworkIdentity, "Network Identity", true, true, true, kNetworkProperties }
		};
	}

	std::span<const ComponentDescriptor> GetSceneComponentDescriptors() noexcept
	{
		return kComponentDescriptors;
	}

	const ComponentDescriptor* FindSceneComponentDescriptor(SceneComponentKind kind) noexcept
	{
		const auto descriptor = std::ranges::find_if(kComponentDescriptors, [kind](const ComponentDescriptor& candidate)
			{
				return candidate.Kind == kind;
			});
		return descriptor == kComponentDescriptors.end() ? nullptr : &(*descriptor);
	}

	std::string_view ToString(PropertyValueKind kind) noexcept
	{
		switch (kind)
		{
		case PropertyValueKind::Bool:
			return "Bool";
		case PropertyValueKind::Int:
			return "Int";
		case PropertyValueKind::Uint64:
			return "Uint64";
		case PropertyValueKind::Float:
			return "Float";
		case PropertyValueKind::Float2:
			return "Float2";
		case PropertyValueKind::Float3:
			return "Float3";
		case PropertyValueKind::Float4:
			return "Float4";
		case PropertyValueKind::String:
			return "String";
		case PropertyValueKind::Path:
			return "Path";
		case PropertyValueKind::Enum:
			return "Enum";
		default:
			return "Unknown";
		}
	}
}
