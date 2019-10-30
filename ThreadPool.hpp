#pragma once

#include <vector>				// std::vector
#include <queue>				// std::queue
#include <memory>				// std::make_shared, std::shared_ptr
#include <thread>				// std::thread
#include <mutex>				// std::mutex, std::unique_lock
#include <condition_variable>	// std::condition_variable
#include <future>				// std::future, std::packaged_task
#include <functional>			// std::bind, std::function
#include <stdexcept>			// std::runtime_error
#include <algorithm>			// std::max

class ThreadPool {
public:
	ThreadPool(size_t poolSize = (std::max)(2u, std::thread::hardware_concurrency()));
	~ThreadPool();

	template<class F, class... Args>
	auto enqueue(F&& f, Args&&... args)
		->std::future<typename std::result_of<F(Args...)>::type>;
	void clearTask();
	size_t getWaitingTaskSize();
	void waitUntilQueueEmpty();

	size_t getPoolSize();
	void setPoolSize(size_t limit);
	size_t getNumAvailable();
	void waitUntilAllIdle();

private:
	void emplace_back_worker(std::size_t worker_number);

	std::size_t pool_size;
	std::atomic<std::size_t> running;

	std::vector< std::thread > workers;
	std::queue< std::function<void()> > tasks;

	// synchronization
	std::condition_variable condition_consumers;
	std::condition_variable condition_task_update;
	std::condition_variable condition_worker_update;

	std::mutex mutex_task;
	std::mutex mutex_worker;
	bool stop;
	bool cleared;
};

// add new work item to the pool
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
-> std::future<typename std::result_of<F(Args...)>::type>
{
	using return_type = typename std::result_of<F(Args...)>::type;

	auto task = std::make_shared< std::packaged_task<return_type()> >(
		std::bind(std::forward<F>(f), std::forward<Args>(args)...)
		);

	std::future<return_type> res = task->get_future();
	{
		std::unique_lock<std::mutex> lock(mutex_task);

		// don't allow enqueueing after stopping the pool
		if (stop)
			throw std::runtime_error("enqueue on stopped ThreadPool");

		tasks.emplace([this, task](){ if (!this->cleared) (*task)(); });
	}

	condition_consumers.notify_one();	
	return res;
}

inline ThreadPool::ThreadPool(size_t poolSize)
	: pool_size(poolSize)
	, running(0)
	, stop(false)
	, cleared(false)
{
	workers.reserve(poolSize);

	for (size_t i = 0; i < poolSize; ++i)
		emplace_back_worker(i);
}

inline ThreadPool::~ThreadPool()
{
	std::unique_lock<std::mutex> lock(mutex_task);
	stop = true;
	condition_consumers.notify_all();
	condition_task_update.notify_all();
	condition_worker_update.notify_all();
	pool_size = 0;

	condition_consumers.wait(lock, [this]{ return this->workers.empty(); });
}

inline void ThreadPool::clearTask()
{
	std::unique_lock<std::mutex> lock(this->mutex_task);

	cleared = true;
	while (tasks.size() > 0)
		tasks.pop();
	cleared = false;

	condition_task_update.notify_all();
}

inline size_t ThreadPool::getWaitingTaskSize()
{
	std::unique_lock<std::mutex> lock(this->mutex_task);
	return tasks.size();
}

inline void ThreadPool::waitUntilQueueEmpty()
{
	std::unique_lock<std::mutex> lock(this->mutex_task);
	this->condition_task_update.wait(lock,
		[this]{ return this->tasks.empty(); });
}

inline size_t ThreadPool::getPoolSize()
{
	return pool_size;
}

inline void ThreadPool::setPoolSize(std::size_t limit)
{
	if (limit < 1)
		limit = 1;

	std::unique_lock<std::mutex> lock_task(this->mutex_task);
	std::unique_lock<std::mutex> lock_worker(this->mutex_worker);

	if (stop)
		return;

	pool_size = limit;
	std::size_t const old_size = this->workers.size();
	if (pool_size > old_size)
	{
		// create new worker threads
		for (std::size_t i = old_size; i != pool_size; ++i)
			emplace_back_worker(i);
	}
	else if (pool_size < old_size)
		// notify all worker threads to start downsizing
		this->condition_consumers.notify_all();
}

inline size_t ThreadPool::getNumAvailable()
{
	return pool_size - running;
}

inline void ThreadPool::waitUntilAllIdle()
{
	std::unique_lock<std::mutex> lock(this->mutex_worker);
	this->condition_worker_update.wait(lock,
		[this]{ return (this->running == 0); });
}

inline void ThreadPool::emplace_back_worker(std::size_t worker_number)
{
	std::unique_lock<std::mutex> lock(this->mutex_worker);

	workers.emplace_back(
		[this, worker_number]
	{
		for (;;)
		{
			std::function<void()> task;

			{
				std::unique_lock<std::mutex> lock(this->mutex_task);
				this->condition_consumers.wait(lock,
					[this, worker_number]{
					return this->stop || !this->tasks.empty()
						|| pool_size < worker_number + 1; });

				// deal with downsizing of thread pool or shutdown
				if (this->stop
					|| (!this->stop && pool_size < worker_number + 1))
				{
					std::thread &last_thread = this->workers.back();
					std::thread::id this_id = std::this_thread::get_id();
					if (this_id == last_thread.get_id())
					{
						// highest number thread exits, resizes the workers
						// vector, and notifies others
						last_thread.detach();
						this->workers.pop_back();
						this->condition_consumers.notify_all();
						this->condition_worker_update.notify_all();

						return;
					}
					else
						continue;
				}
				else if (!this->tasks.empty())
				{
					task = std::move(this->tasks.front());
					this->tasks.pop();
					if (this->tasks.empty())
						condition_task_update.notify_all();

					std::atomic_fetch_add_explicit(&this->running,
						std::size_t(1),
						std::memory_order_relaxed);

					condition_worker_update.notify_all();
				}
				else
					continue;
			}

			task();

			std::atomic_fetch_sub_explicit(&this->running,
				std::size_t(1),
				std::memory_order_relaxed);

			condition_worker_update.notify_all();
		}
	}
	);

	this->condition_worker_update.notify_all();
}
