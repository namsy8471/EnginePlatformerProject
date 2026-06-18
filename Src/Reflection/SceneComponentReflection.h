#pragma once

#include "Scene/SceneComponents.h"

#include <span>
#include <string_view>

namespace Reflection
{
	enum class PropertyValueKind : uint8_t
	{
		Bool,
		Int,
		Uint64,
		Float,
		Float2,
		Float3,
		Float4,
		String,
		Path,
		Enum
	};

	struct PropertyDescriptor
	{
		std::string_view Name;
		PropertyValueKind ValueKind = PropertyValueKind::String;
	};

	struct ComponentDescriptor
	{
		SceneComponentKind Kind = SceneComponentKind::Mesh;
		std::string_view Name;
		bool CanAdd = true;
		bool CanRemove = true;
		bool CanDisable = true;
		std::span<const PropertyDescriptor> Properties;
	};

	[[nodiscard]] std::span<const ComponentDescriptor> GetSceneComponentDescriptors() noexcept;
	[[nodiscard]] const ComponentDescriptor* FindSceneComponentDescriptor(SceneComponentKind kind) noexcept;
	[[nodiscard]] std::string_view ToString(PropertyValueKind kind) noexcept;
}
