#pragma once

#include "ECS/ECSWorld.h"

#include <utility>

namespace ECS
{
	template <typename... ComponentTypes>
	class View
	{
	public:
		explicit View(ECSWorld& world) noexcept
			: m_World(world)
		{
		}

		template <typename FunctionType>
		void Each(FunctionType&& function)
		{
			m_World.ForEach<ComponentTypes...>(std::forward<FunctionType>(function));
		}

	private:
		ECSWorld& m_World;
	};
}
