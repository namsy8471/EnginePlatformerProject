#include "Jobs/SceneCommandBuffer.h"

#include <utility>

namespace Jobs
{
	void SceneCommandBuffer::Enqueue(std::function<void()> command)
	{
		if (!command)
		{
			return;
		}

		std::scoped_lock lock(m_Mutex);
		m_Commands.push_back(std::move(command));
	}

	void SceneCommandBuffer::ExecuteAndClear()
	{
		std::vector<std::function<void()>> commands;
		{
			std::scoped_lock lock(m_Mutex);
			commands.swap(m_Commands);
		}

		for (auto& command : commands)
		{
			command();
		}
	}

	void SceneCommandBuffer::Clear()
	{
		std::scoped_lock lock(m_Mutex);
		m_Commands.clear();
	}

	bool SceneCommandBuffer::Empty() const
	{
		std::scoped_lock lock(m_Mutex);
		return m_Commands.empty();
	}
}
