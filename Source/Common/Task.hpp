#pragma once
#include "../pch.hpp"
/* task typedefs */
typedef std::packaged_task<void()> void_task_type;
typedef std::shared_ptr<void_task_type> void_task_ptr_type;
/* task concepts */
template<typename T> concept void_task_container = requires(T t) {
	{ t.push(void_task_ptr_type()) };
	{ t.pop() };
	{ t.front() } -> std::convertible_to<void_task_ptr_type>;
};
/* container types */
typedef std::queue<void_task_ptr_type> task_queue_container_fifo;
class task_queue_container_lifo : public std::stack<void_task_ptr_type> {
public:
	using std::stack<void_task_ptr_type>::stack;
	auto& front() { return top(); }
};
/* task queue */
template<void_task_container _Tc> class task_queue {
public:
	typedef _Tc container_type;
	auto push(auto func, auto&&... args) {
		typedef decltype(func(args...)) return_type;
		auto task = std::make_shared<std::packaged_task<return_type()>>(std::bind(func, args...));
		auto future = task->get_future();
		tasks.push(std::make_shared<void_task_type>([task = std::move(task)]() { (*task)(); /* task dtor */ }));
		/* task in this scope is no longer valid. ownership is now transfered to the smart pointer */
		return future;
	}
protected:
	std::optional<void_task_ptr_type> pop() {
		if (tasks.size()) {
			void_task_ptr_type ptr = std::move(tasks.front()); /* actual dtor will be handled by caller */
			tasks.pop();
			return ptr;
		}
		return {};
	}
private:
	container_type tasks;
};
/* task threadpool */
template<void_task_container _Tc> class task_threadpool : public task_queue<_Tc> {
public:
	typedef task_queue<_Tc> queue_type;
	using queue_type::container_type;

	task_threadpool(size_t num_threads = -1) {
		if (num_threads == -1)
			num_threads = std::thread::hardware_concurrency();
		threads.resize(num_threads);
		for (auto& thread : threads) thread = std::jthread(std::bind(&task_threadpool<_Tc>::worker_job, this));
	}	
	auto push(auto&& func, auto&&... args) {
		{
			std::lock_guard lock(worker_mutex);
			queue_type::push(func, args...);
		}
		new_task_cv.notify_one();
	}
	void stop_and_join() {
		running = false;
		new_task_cv.notify_all();
		for (auto& thread : threads) if (thread.joinable()) thread.join();
	}
	~task_threadpool() {
		stop_and_join();
	}
private:
	std::mutex worker_mutex;
	std::vector<std::jthread> threads;
	std::condition_variable new_task_cv;
	bool running = true;
	void worker_job() {
		while (running) {
			std::unique_lock lock(worker_mutex);
			new_task_cv.wait(lock);
			auto task = queue_type::pop();
			lock.unlock();
			if (task.has_value()) (*task.value())();
		}
	}
};