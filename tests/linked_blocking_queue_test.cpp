#include "pch.h"
#include "../concurrent_data_structures/thread_pool.h"
#include "../concurrent_data_structures/linked_blocking_queue.h"
#include <vector>
#include <thread>
#include <algorithm>
#include <numeric>
#include <string>
#include <chrono>

using namespace hungbiu;
using namespace std;

/*
 * Single producer and single consumer
 * queue<int>
 * Confirm that the queue basically work
*/
TEST(LnkBlkQueue, SPSC) {
	const size_t N = 10;
	vector<int> inputs, outputs;
	inputs.resize(N);
	iota(inputs.begin(), inputs.end(), 1);
	outputs.reserve(inputs.size());

	linked_blocking_queue<int> lbq;

	thread t1{ [&]() {
		for_each(inputs.cbegin(), inputs.cend(),
			[&](auto e) { lbq.push(e); });
	} };

	thread t2{ [&]() {
		for (auto i = 0u; i < inputs.size(); ++i) {
			outputs.push_back(lbq.pop());
		}
	} };

	t1.join();
	t2.join();

	ASSERT_EQ(inputs, outputs);
}

/* 
 * Multiple producers and multiple consumers
 * queue<int> for trivially copiable types
*/
TEST(LnkBlkQueue, MPMC_int) {
	const size_t Diff = 1000;
	const size_t Producers_N = 4;
	const size_t Consumers_N = 4;
	const size_t Sum = Diff * Producers_N;
	atomic<int> begin = 0;
	atomic<size_t> counter = 0;

	// vector<int> of [0, Sum)
	vector<int> inputs;
	inputs.resize(Sum);
	iota(inputs.begin(), inputs.end(), begin.load());

	linked_blocking_queue<int> lbq;

	// Build pool of producers
	auto producer_work = [&]() {
		// Build a vector<int> of [beg, beg + Diff)
		auto b = begin.load();
		while (!begin.compare_exchange_weak(b, b + Diff))
			;
		auto beg = inputs.cbegin() + b;
		auto end = beg + Diff;
		vector<int> data(beg, end);

		// Enqueue
		for_each(data.cbegin(), data.cend(),
			[&](auto e) { lbq.push(e); });
	};
	thread_array<Producers_N> producers(producer_work);

	// Build pool of consumers
	spinlock spnlk;
	vector<int> outputs;
	outputs.reserve(Sum);
	auto outputs_add = [&](int e) {
		lock_guard lk{ spnlk };
		outputs.push_back(e);
	};

	auto consumer_work = [&]() {
		size_t cnt = 0u;
		while (counter.load() < Sum) {
			auto opt_e = lbq.try_pop();
			if (!opt_e) continue;

			outputs_add(opt_e.value());
			while (!counter.compare_exchange_weak(cnt, cnt + 1))
				;
		}
	};
	thread_array<Consumers_N> consumers(consumer_work);

	producers.join_all();
	consumers.join_all();

	sort(outputs.begin(), outputs.end());

	EXPECT_TRUE(lbq.empty());
	EXPECT_EQ(inputs.size(), outputs.size());
	ASSERT_EQ(inputs, outputs);
}

/*
 * Multiple producers and multiple consumers
 * queue<unique_ptr<int>> for move-only types
*/
TEST(LnkBlkQueue, MPMC_unique_ptr) {
	const size_t Diff = 1000;
	const size_t Producers_N = 4;
	const size_t Consumers_N = 4;
	const size_t Sum = Diff * Producers_N;
	atomic<int> begin = 0;
	atomic<size_t> counter = 0;

	// vector<int> of [0, Sum)
	vector<int> inputs;
	inputs.resize(Sum);
	iota(inputs.begin(), inputs.end(), begin.load());

	linked_blocking_queue<unique_ptr<int>> lbq;

	// Build pool of producers
	auto producer_work = [&]() {
		// Build a vector<int> of [beg, beg + Diff)
		auto b = begin.load();
		while (!begin.compare_exchange_weak(b, b + Diff))
			;
		auto beg = inputs.cbegin() + b;
		auto end = beg + Diff;
		vector<int> data(beg, end);

		// Enqueue
		for_each(data.cbegin(), data.cend(),
			[&](auto e) { lbq.push(make_unique<int>(e)); });
	};
	thread_array<Producers_N> producers(producer_work);

	// Build pool of consumers
	spinlock spnlk;
	vector<int> outputs;
	outputs.reserve(Sum);
	auto outputs_add = [&](int e) {
		lock_guard lk{ spnlk };
		outputs.push_back(e);
	};

	auto consumer_work = [&]() {
		size_t cnt = 0u;
		while (counter.load() < Sum) {
			auto opt_e = lbq.try_pop();
			if (!opt_e) continue;

			outputs_add(*opt_e.value());
			while (!counter.compare_exchange_weak(cnt, cnt + 1))
				;
		}
	};
	thread_array<Consumers_N> consumers(consumer_work);

	producers.join_all();
	consumers.join_all();

	sort(outputs.begin(), outputs.end());

	EXPECT_TRUE(lbq.empty());
	EXPECT_EQ(inputs.size(), outputs.size());
	ASSERT_EQ(inputs, outputs);
}

TEST(LnkBlkQueue, MISC)
{
	linked_blocking_queue<string> lbq;

	auto producer = thread{ [&]() {
		using namespace std::chrono_literals;
		this_thread::sleep_for(2s);
		lbq.push("done");
	} };

	const size_t Consumers_N = 4;
	atomic<bool> flag{ false };
	thread_array<Consumers_N> consumers{ [&]() {		
		while ( !flag.load(memory_order_relaxed) ) {
			auto r = lbq.try_pop();
			if (r && *r == "done") {
				bool b = false;
				ASSERT_TRUE(flag.compare_exchange_strong(b, true));
				break;
			}
		}
		return;
	} };

	producer.join();
	consumers.join_all();

	ASSERT_TRUE(lbq.empty());
}