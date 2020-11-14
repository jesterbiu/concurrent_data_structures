#include "pch.h"
#include "../concurrent_data_structures/array_blocking_queue.h"
#include "../concurrent_data_structures/thread_pool.h"
#include "../concurrent_data_structures/spinlock.h"
#include <thread>
#include <vector>
#include <memory>
#include <mutex>
using namespace std;
using namespace hungbiu;

/*
 * SPSC
*/
TEST(ArrBlkQueue, SPSC) {
	const size_t N = 1000;
	vector<int> inputs, outputs;
	inputs.resize(N);
	iota(inputs.begin(), inputs.end(), 1);
	outputs.reserve(inputs.size());

	array_blocking_queue<int> abq(256);

	// Pop from an empty queue
	int v;
	ASSERT_FALSE(abq.try_pop(v));

	// Producer thread
	thread t1{ [&]() {
		for_each(inputs.cbegin(), inputs.cend(),
			[&](auto e) { abq.push(e); });
	} };

	// Consumer thread
	thread t2{ [&]() {
		int v;
		for (auto i = 0u; i < inputs.size(); ++i) {
			abq.pop(v);
			outputs.push_back(v);
		}
	} };

	t1.join();
	t2.join();

	ASSERT_FALSE(abq.try_pop(v));
	ASSERT_EQ(inputs, outputs);
}

/*
 * MPMC
*/
TEST(ArrBlkQueue, MPMC_unique_ptr) {
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

	array_blocking_queue<unique_ptr<int>> abq(256);

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
			[&](auto e) { abq.emplace(make_unique<int>(e)); });
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
		while (counter.load() < Sum) {
			unique_ptr<int> v;
			if (!abq.try_pop(v)) continue;

			outputs_add(*v);
			counter.fetch_add(1);
		}
	};
	thread_array<Consumers_N> consumers(consumer_work);

	producers.join_all();
	consumers.join_all();

	// There is no element in the queue
	unique_ptr<int> v;
	ASSERT_FALSE(abq.try_pop(v));

	sort(outputs.begin(), outputs.end());
	EXPECT_EQ(inputs.size(), outputs.size());
	ASSERT_EQ(inputs, outputs);
}