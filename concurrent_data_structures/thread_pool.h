#pragma once
#include <thread>
#include <cstddef>
#include <array>
#include <numeric>

namespace hungbiu
{
	template<std::size_t N>
	struct thread_array
	{
		std::array<std::thread, N> data_;

		template<typename F>
		thread_array(F func)
		{
			std::for_each(data_.begin(), data_.end(),
				[&](std::thread& t) { t = std::thread{ func }; });
		}
		thread_array(const thread_array& oth) = delete;
		thread_array& operator=(const thread_array& rhs) = delete;
		~thread_array()
		{
			join_all();
		}
		void join_all() 
		{
			std::for_each(data_.begin(), data_.end(),
				[](std::thread& t) { if (t.joinable()) t.join(); });
		}
	};
}
