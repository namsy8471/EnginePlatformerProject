#pragma once

#include "Math/Camera.h"
#include "Scene/Scene.h"
#include "Scene/SceneRenderState.h"

namespace Samples::Spider
{
	struct LoadResult
	{
		EntityId SpiderEntity = InvalidEntityId;
	};

	[[nodiscard]] bool Load(Scene& scene, SceneRenderState& renderState, Camera& camera, LoadResult& result);
}
