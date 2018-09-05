#pragma once

#include <vector>				// std::vector
#include <queue>				// std::queue
#include <memory>				// std::make_shared, std::shared_ptr
#include <thread>				// std::thread
#include <mutex>				// std::mutex, std::unique_lock
#include <condition_variable>	// std::condition_variable
#include <future>				// std::future, std::packaged_task
#include <functional>			// std::bind
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
	void waitUntilQueueEmpty();

	size_t getSize();
	void setSize(size_t limit);
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
