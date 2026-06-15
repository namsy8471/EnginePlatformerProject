#pragma once

#include <cstdint>

using EntityId = uint32_t;
constexpr EntityId InvalidEntityId = 0;

struct SceneEntity
{
	EntityId Id = InvalidEntityId;
};
