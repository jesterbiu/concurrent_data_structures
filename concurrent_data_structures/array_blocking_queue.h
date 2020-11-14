#pragma once
#include <cstddef>
#include <atomic>
#include <new>
#include <type_traits>
#include <cstdlib>
#include <cassert>

namespace hungbiu
{
	template<typename T>
	class array_blocking_queue
	{
		#define ALIGN_REQ alignas(std::hardware_destructive_interference_size)

		using storage_t = std::aligned_storage_t<sizeof(T), alignof(T)>;
		template<typename U>
		struct slot_t
		{
			ALIGN_REQ std::atomic<std::size_t> turn_{ 0 };
			storage_t val_;
			~slot_t()
			{
				if (has_val()) {
					destroy();
				}
			}			
			bool has_val() noexcept 
			{
				return turn_.load(std::memory_order_acquire) & 1;
			}
			template <typename ...Args> 
			void construct(Args&&... args) 
				noexcept(std::is_nothrow_constructible<U, Args&&...>::value)
			{				
				new (&val_) U(std::forward<Args>(args)...);
			}
			void destroy() noexcept(std::is_nothrow_destructible<U>::value)
			{
				reinterpret_cast<U*>(&val_)->~U();
			}
		};

		const std::size_t capacity_;
		slot_t<T>* array_;
		ALIGN_REQ std::atomic<std::size_t> head_  { 0 };
		ALIGN_REQ std::atomic<std::size_t> tail_  { 0 };
		
		slot_t<T>* allocate()
		{
			auto p = malloc(sizeof(slot_t<T>) * capacity_);
			if (!p) throw std::bad_alloc{};
			return static_cast<slot_t<T>*>(p);
		}
		void construct()
		{
			for (auto i = 0u; i < capacity_; ++i) {
				new (&array_[i]) slot_t<T>();
			}
		}
		void destroy()
		{
			for (size_t i = 0; i < capacity_; ++i) {
				array_[i].~slot_t<T>();
			}
		}
		void deallocate()
		{
			free(static_cast<void*>(array_));
		}
		std::size_t get_idx(std::size_t ticket) const noexcept
		{
			return ticket & (capacity_ - 1);
		}
		std::size_t get_write_turn(std::size_t ticket) const noexcept
		{
			return ticket / capacity_ * 2;
		}
		std::size_t get_read_turn(std::size_t ticket) const noexcept
		{
			return ticket / capacity_ * 2 + 1;
		}
		void done_writing(slot_t<T>& slot, const std::size_t write_ticket)
		{
			const auto read_turn = get_read_turn(write_ticket);
			slot.turn_.store(read_turn, std::memory_order_release);
		}
		void done_reading(slot_t<T>& slot)
		{
			slot.turn_.fetch_add(1, std::memory_order_release);
		}
	public:
		// ctor
		array_blocking_queue(std::size_t capacity) :
			capacity_(capacity)
		{
			assert(capacity_ > 1);
			assert(capacity_ % 2 == 0);

			array_ = allocate();
			construct();
		}
					
		// dtor
		~array_blocking_queue()
		{
			destroy();
			deallocate();
		}
		
		// Delete copy constructor and assignment operator
		array_blocking_queue(const array_blocking_queue&) = delete;
		array_blocking_queue& operator=(const array_blocking_queue&) = delete;
		
		// Modifiers
		/*
		* try_push/try_emplace:
		* Try again if if there is other threads have succeed
		* Return false if it's not out turn and no other thread has done enqueue.
		* Pseudo-code:
		* get a ticket reading tail_,
		*	   enqueue_position = ticket % capacity
		*	   turn_to_enqueue = ticket / capacity * 2 //even number
		* if (position.turn == turn_to_enqueue && ticket == tail)
		*     enqueue; return true; // SUCCESS
		* if (position.turn == turn_to_enqueue && ticket != tail)
		*     continue; // SOMEONE CONSTRUCTING, GET ANOTHERT TICKET
		* if (position.turn != turn_to_enqueue && ticket == tail_)
		*     return false; // NOT OUR TURN YET, OUT
		* if (position.turn != turn_to_enqueue && ticket != tail_)
		*     continue; // // SOMEONE HAVE DONE CONSTRUCTED, GET ANOTHER TICKET
		**/
		template<typename ...Args,
			typename = std::enable_if_t<std::is_constructible_v<T, Args&&...>> >
		bool try_emplace(Args&&... args)
			noexcept(std::is_nothrow_constructible<T, Args&&...>::value)
		{
			// Acquire a write ticket for trying
			auto write_ticket = tail_.load(std::memory_order_acquire);
			for (;;) {
				auto& slot = array_[get_idx(write_ticket)];
	
				// If it's probably my turn,
				if (get_write_turn(write_ticket) == slot.turn_.load(std::memory_order_acquire))
				{
					// ...no thread is competing, construct data
					if (tail_.compare_exchange_strong(write_ticket,
													  write_ticket + 1,
													  std::memory_order_acq_rel)) {
						slot.construct(std::forward<Args>(args)...);
						done_writing(slot, write_ticket);
						return true;
					}
					// ...another thread already started constructing data,
					// wait for its completion and go get another ticket
					else { continue; }
				}
				// It's not my turn,
				else {
					// ...get another ticket for trying
					const auto old_ticket = write_ticket;
					write_ticket = tail_.load(std::memory_order_acquire);

					// ...other threads have completed constructing data, old_ticket expired
					// try the new ticket
					if (old_ticket != write_ticket) { continue; }
					// ...ticket valid, but not my turn
					else { return false; }
				}
			} // end of for loop
		}
		template<typename ...Args,
			typename = std::enable_if_t<std::is_constructible_v<T, Args&&...>> >
		void emplace(Args&&... args)
			noexcept(std::is_nothrow_constructible<T, Args&&...>::value)
		{		
			// Acquire a write ticket
			const auto write_ticket = tail_.fetch_add(1, std::memory_order_acq_rel);
			auto& slot = array_[get_idx(write_ticket)];
			const auto turn = get_write_turn(write_ticket);

			// Wait for my turn
			while (turn != slot.turn_.load(std::memory_order_acquire))
				;

			// Construct data
			slot.construct(std::forward<Args>(args)...);
			done_writing(slot, write_ticket);
		}
		bool try_push(const T& val)
		{
			return try_emplace(val);
		}
		bool try_push(T&& val)
		{
			return try_emplace(std::forward<T>(val));
		}
		void push(const T& val)
		{
			emplace(val);
		}
		void push(T&& val)
		{
			emplace(std::forward<T>(val));
		}


		bool try_pop(T& val)
		{
			// Acquire a read ticket for trying
			auto read_ticket = head_.load(std::memory_order_acquire);
			for (;;) {
				auto& slot = array_[get_idx(read_ticket)];

				// If it's probably my turn
				if (get_read_turn(read_ticket) == slot.turn_.load(std::memory_order_acquire)) {
					// ...no thread is competing, start reading
					if (head_.compare_exchange_strong(read_ticket,
													  read_ticket + 1,
													  std::memory_order_acq_rel)) {
						val = std::move(reinterpret_cast<T&&>(slot.val_));
						slot.destroy();
						done_reading(slot);
						return true;
					}
					// ...another thread already started reading,
					// wait for its completion and get another ticket
					else { continue; }
				}
				// It's not my turn
				else
				{
					const auto old_ticket = read_ticket;
					read_ticket = head_.load(std::memory_order_acquire);

					// ...other thread has completed reading the data, read_ticket expired
					// try the new ticket
					if (read_ticket != old_ticket) { continue; }
					// ...ticket valid but not my turn
					else { return false; }
				}
			}// end of for loop
		}

		void pop(T& val)
		{
			// Acquire read ticket
			const auto read_ticket = head_.fetch_add(1, std::memory_order_acq_rel);
			auto& slot = array_[get_idx(read_ticket)];
			const auto turn = get_read_turn(read_ticket);

			// Wait for my turn
			while (turn != slot.turn_.load(std::memory_order_acquire))
				; //yield()?

			// Read data
			val = std::move(reinterpret_cast<T&&>(slot.val_));
			slot.destroy();
			done_reading(slot);
		}

	}; // end of class
}
