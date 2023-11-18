#pragma once
#include "../pch.hpp"
/* functional utilties*/
typedef std::function<void()> VoidFunction;
typedef std::shared_ptr<VoidFunction> VoidFunctionPtr;
VoidFunctionPtr make_function_ptr(auto func, auto&&... args) {
	typedef decltype(func(args...)) return_type;
	static_assert(std::is_void_v(return_type));
	return std::make_shared<std::function<return_type()>>(std::bind(func, args...));
}
/* task typedefs */
typedef std::packaged_task<void()> VoidTask;
typedef std::shared_ptr<VoidTask> VoidTaskPtr;
VoidTaskPtr make_task_ptr(auto&& func, auto&&... args) {
	typedef decltype(func(args...)) return_type;	
	return std::make_shared<std::packaged_task<return_type()>>(std::bind(std::move(func), args...));
}
/* task concepts */
template<typename T> concept VoidTaskContainer = requires(T t) {
	{ t.push(VoidTaskPtr()) };
	{ t.pop() };
	{ t.front() } -> std::convertible_to<VoidTaskPtr>;
};
/* container types */
typedef std::queue<VoidTaskPtr> TaskQueueFIFOContainer;
class TaskQueueLIFOContainer : public std::stack<VoidTaskPtr> {
public:
	using std::stack<VoidTaskPtr>::stack;
	auto& front() { return top(); }
};
/* task queue */
template<VoidTaskContainer _Tc> class TaskQueue {
public:
	typedef _Tc container_type;
	auto push(auto&& func, auto&&... args) {
		typedef decltype(func(args...)) return_type;
		auto task = make_task_ptr(func, args...);
		auto future = task->get_future();
		tasks.push(std::make_shared<VoidTask>([task = std::move(task)]() { (*task)(); /* task dtor */ }));
		/* task in this scope is no longer valid. ownership is now transfered to the smart pointer */
		return future;
	}
	size_t size() {
		return tasks.size();
	}
protected:
	std::optional<VoidTaskPtr> pop() {
		if (tasks.size()) {
			VoidTaskPtr ptr = std::move(tasks.front()); /* actual dtor will be handled by caller */
			tasks.pop();
			return ptr;
		}
		return {};
	}
private:
	container_type tasks;
};
/* task threadpool */
template<VoidTaskContainer _Tc> class TaskThreadPool : public TaskQueue<_Tc> {
public:
	typedef TaskQueue<_Tc> queue_type;
	using queue_type::container_type;

	TaskThreadPool(size_t num_threads = -1) {
		if (num_threads == -1)
			num_threads = std::thread::hardware_concurrency();
		threads.resize(num_threads);
		for (auto& thread : threads) thread = std::jthread(std::bind(&TaskThreadPool<_Tc>::worker_job, this));
	}	
	auto push(auto&& func, auto&&... args) {
		std::scoped_lock lock(worker_mutex);
		auto future = queue_type::push(func, args...);		
		new_task_cv.notify_one();
		return future;
	}
	void wait_and_join() {
		wait_for_jobs = true, running = false;
		new_task_cv.notify_all();
		for (auto& thread : threads) if (thread.joinable()) thread.join();
	}
	void stop_and_join() {
		wait_for_jobs = running = false;
		new_task_cv.notify_all();
		for (auto& thread : threads) if (thread.joinable()) thread.join();
	}
	~TaskThreadPool() {
		wait_and_join();
	}
private:
	std::mutex worker_mutex;
	std::vector<std::jthread> threads;
	std::condition_variable new_task_cv;
	bool running = true;
	bool wait_for_jobs = true;
	void worker_job() {
		while (running || (wait_for_jobs && queue_type::size() > 0)) {
			std::unique_lock lock(worker_mutex);
			new_task_cv.wait(lock, [this] { return !running || queue_type::size() > 0; });
			auto task = queue_type::pop();
			lock.unlock();
			if (task.has_value()) (*task.value())();
		}
	}
};

typedef TaskThreadPool<TaskQueueFIFOContainer> DefaultTaskThreadPool;