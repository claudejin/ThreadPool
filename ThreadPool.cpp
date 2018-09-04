#include "ThreadPool.h"

ThreadPool::ThreadPool(size_t poolSize)
	: pool_size(poolSize)
	, running(0)
	, stop(false)
{
	workers.reserve(poolSize);

	for (size_t i = 0; i < poolSize; ++i)
		emplace_back_worker(i);
}

ThreadPool::~ThreadPool()
{
	std::unique_lock<std::mutex> lock(mutex_task);
	stop = true;
	condition_consumers.notify_all();
	condition_task_update.notify_all();
	condition_worker_update.notify_all();
	pool_size = 0;

	condition_consumers.wait(lock, [this]{ return this->workers.empty(); });
}

void ThreadPool::clearTask()
{
	std::unique_lock<std::mutex> lock(this->mutex_task);

	while (tasks.size() > 0)
		tasks.pop();

	condition_task_update.notify_all();
}

void ThreadPool::waitUntilQueueEmpty()
{
	std::unique_lock<std::mutex> lock(this->mutex_task);
	this->condition_task_update.wait(lock,
		[this]{ return this->tasks.empty(); });
}

size_t ThreadPool::getSize()
{
	return pool_size;
}

void ThreadPool::setSize(std::size_t limit)
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

size_t ThreadPool::getNumAvailable()
{
	return pool_size - running;
}

void ThreadPool::waitUntilAllIdle()
{
	std::unique_lock<std::mutex> lock(this->mutex_worker);
	this->condition_worker_update.wait(lock,
		[this]{ return this->getNumAvailable() == this->getSize(); });
}

void ThreadPool::emplace_back_worker(std::size_t worker_number)
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
				}
				else
					continue;
			}

			std::atomic_fetch_add_explicit(&this->running,
				std::size_t(1),
				std::memory_order_relaxed);

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
