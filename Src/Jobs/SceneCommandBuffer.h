#pragma once

#include <functional>
#include <mutex>
#include <vector>

namespace Jobs
{
	class SceneCommandBuffer
	{
	public:
		void Enqueue(std::function<void()> command);
		void ExecuteAndClear();
		void Clear();
		[[nodiscard]] bool Empty() const;

	private:
		mutable std::mutex m_Mutex;
		std::vector<std::function<void()>> m_Commands;
	};
}
