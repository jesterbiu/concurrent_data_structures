// 把optional改成union或者干脆改成可以默认构造的T

#pragma once
#include <cstddef>  // size_t
#include <optional> // optional<T>
#include <atomic>   // atomic<size_type>
#include <memory>   // shared_ptr<node>
#include <utility>  // move()
#include <mutex>    // scoped_lock
#include <condition_variable>
#include <cassert>
#include <type_traits>
#include <cstring>
#include <cstdlib>  // Use malloc for allocator
#include "spinlock.h" // spinlock


namespace hungbiu {
    static constexpr auto Target_Cache_Line_Size = 64u;
    #define ALIGN_REQ alignas(Target_Cache_Line_Size)

    template <typename T>
    class linked_blocking_queue
    {
    public:
        // Container template typedef
        using value_type = T;
        using size_type = std::size_t;

    private:
        struct node;
        using atomic_ptr = std::atomic<node*>;
        using lock_t = spinlock;

        /* 
         * Represents a node in the underlying linked list of the queue.
         * The only ctor has no args, thus its contents need be specified later.
         * The default construct is never called and val_ is contructed using placement new,
         * hence it doesn't require T can be default constructed to use this container.
        */
        struct node
        {
            T       val_;
            node*   next_;
            
            node(const T& val) :
                val_(val), 
                next_(nullptr) {}
            node(T&& val) :
                val_(std::move(val)), 
                next_(nullptr) {}
            node(const node&) = delete;
            ~node() = default;
            node& operator=(const node&) = delete;            
        };
        
        /*
         * Represent either end of the queue. 
         * It packs a pointer to an node and a spinlock, 
         * which needs to be locked before modifying the pointer.
         * 
         * It aligns to cache line size to prevent false sharing.
        */
        struct ALIGN_REQ end
        {
            mutable lock_t  lock_;
            atomic_ptr      ptr_;
        };
        
        // Members
        ALIGN_REQ atomic_ptr free_list_{ nullptr };
        end front_;
        end back_;
        std::condition_variable_any cv_;
        
        /*
         * @brief   push the newly released node to the front of the free_list_
         *          using CAS
         * @param   empty_node  pointer to the node released
        */
        void free_node(node* p) noexcept
        {
            // Does not call destructor. If T is managing some resource
            // the resource won't be released.
            // Swap the freed node to the head of free_list_
            p->next_ = free_list_.load(std::memory_order_acquire);
            while ( !free_list_.compare_exchange_weak( p->next_,
                                                       p,
                                                       std::memory_order_acq_rel) ) ;
        }
        
        /*
         * @brief   allocate a new node from this->free_list_
         * @return  return the first node of the free_list_; 
         *          if there is no node available, return nullptr
         * 
         * Try to pop a previously cached node from free_list_ using CAS.
         * Using linked-list-based queue has a problem that dynamic allocation could 
         * slow down its perfermance. Caching popped nodes may help relieve that.
        */
        node* alloc_from_free_list() noexcept
        {
            node* alloc = free_list_.load(std::memory_order_acquire);
            while ( alloc &&
                    !free_list_.compare_exchange_weak( alloc,           // expect
                                                       alloc->next_,    // desire
                                                       std::memory_order_acq_rel )) ;            
            return alloc;
        }

        /*
         * @brief   allocate a new node from allocator
         * @return  pointer to the allocated node
        */
        static node* alloc_from_allocator()
        {
            return static_cast<node*>(malloc(sizeof(node)));
        }

        /* 
         * @brief   node's factory function
         * @param   val     data to be added
         * @return  return pointer to the allocated node
         * @exception   it may throw when both free_list_ and ::new  
         *              are exhausted.
         * It first tries to allocate from free_list_ to get cached nodes, 
         * then use ::new if fails.
        */        
        node* alloc_node() 
        {
            // Allocate a new node
            // but doesn't construct it
            node* new_node = alloc_from_free_list();
            if (!new_node) {                
                new_node = alloc_from_allocator();
            }
            std::memset(new_node, 0, sizeof(node));
            return new_node;
        }

        /*
         * @brief   append new node to the end of the list
         * 
         * After insertion, notify one of the thread (if there is any) 
         * waiting on the condition variable.
         * And it doesn't need to acquire front.lock. If when the queue 
         * is emtpy, no thread will modify front.
        */
        template<typename U, 
                 typename = std::enable_if_t<std::is_constructible_v<T, U&&>>>
        void insert(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>)
        {
            // Alloc a new node to be the new tail
            // Move this out of critcal section 
            // because memory allocation may be slow
            auto new_node = alloc_node();            
            {
                std::lock_guard lk_back{ back_.lock_ };
                auto tail = back_.ptr_.load(std::memory_order_acquire);

                // Construct old tail (possibly time-consuming)
                new (tail) node(std::forward<U>(value));
                // new_node->next_ = nullptr;

                // Modify back.ptr to append new_node to the tail of the queue
                tail->next_ = new_node;
                back_.ptr_.store(new_node, std::memory_order_release);
            }        

            // Notify one waiting thread
            cv_.notify_one();
        }

        /*
         * @brief   impl of public pop()
         * @param   front_lk    unique lock on front.lock
         * @param   ret         value to return
         * @exception   ret.emplace() may throw calling T::ctor(T oth)
         * 
         * Caller has to own the front.lock and confirm queue is not emtpy
         * before calling. Since the return value will be copy-constructed
         * holding the front_lk, one should not store a type T which is hard 
         * to be moved from or too big to be copied. Otherwise, the copy may
         * decrease the pop()'s throughput.  
        */
        T pop(std::unique_lock<lock_t> front_lk) 
            noexcept(std::is_nothrow_copy_constructible_v<T> ||
                     std::is_nothrow_move_constructible_v<T> )
        {
            // Confirm that caller owns front_lk and queue is not emtpy
            assert(!empty());

            // Else acquire the head node
            auto old = front_.ptr_.load(std::memory_order_acquire);

            // Construct return value from old
            // If this shall throw, the queue stay un-modified,
            // and front_lk's dtor will be called before exiting the function frame
            T e = std::move(old->val_);

            // Pop node
            front_.ptr_.store(old->next_, std::memory_order_release);

            // Release front.lock here
            front_lk.unlock();

            // Delete old_front
            free_node(old);

            return e;
        }
        
    public:        
        /*
         * @brief   default constructor
         * 
         * Allocate a dumb node as a head node
         * which may throw std::bad_alloc.
        */
        linked_blocking_queue() 
        {
            auto p = alloc_node();
            front_.ptr_.store(p, std::memory_order_release);
            back_.ptr_.store(p, std::memory_order_release);
        }

        linked_blocking_queue(const linked_blocking_queue&) = delete;

        /*
         * @brief    destructor
         * 
         * Release all nodes back to the allocator.
        */
        ~linked_blocking_queue()
        {
            std::scoped_lock slk{ front_.lock_, back_.lock_ };

            auto delete_list = [](node* p) {
                while (p) {
                    auto tmp = p;
                    p = p->next_;
                    tmp->~node();
                    free( static_cast<void*>(tmp) );
                }
            };

            // Delete nodes enqueued
            node* p = front_.ptr_.exchange(nullptr, std::memory_order_acq_rel);
            delete_list(p);

            // Delete nodes freed
            p = free_list_.exchange(nullptr, std::memory_order_acq_rel);
            delete_list(p);
        }

        linked_blocking_queue& operator=(const linked_blocking_queue&) = delete;

        // Capacity
        /*
         * @brief check if the queue is emtpy
        */
        bool empty() const noexcept
        {
            return !front_.ptr_.load(std::memory_order_acquire)->next_;
        }

        // Modifiers
        /*
         * @brief push new data to the tail of the queue
         * @param   value   data to be pushed
         * @exception   memory allocation for the node and 
                        copying the value may throw std::bad_alloc
        */
        void push(const T& value)
        {
            insert(value);
        }
        void push(T&& value)
        {
            insert(std::forward<T>(value));
        }        

        /*
         * @brief non-blocking pop
         * @return  if success, the value will be store in optional<T>,
         *          else the value inside will not be constructed.
         * @exception   copy constructor or move constructor of T may throw
         * 
         * Return immediately if the queue is empty.
        */
        [[nodiscard]] std::optional<T> try_pop()
        {
            std::optional<T> ret{};
            std::unique_lock front_lk{ front_.lock_ };
            if (empty()) return ret;
            ret = std::move( pop(std::move(front_lk)) );
            return ret;
        }
        /*
         * @brief   blocking pop
         * @return  value in the front node
         * @exception   copy constructor or move constructor of T may throw
         * 
         * Block if the queue is empty.
        */
        [[nodiscard]] T pop()
        {
            std::unique_lock front_lk{ front_.lock_ };
            cv_.wait(front_lk,
                [&]() { return !empty(); });
            return pop( std::move(front_lk) );
        }
    };

};