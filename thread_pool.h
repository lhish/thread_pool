//
// Created by lhy on 25-8-7.
//

#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <list>
#include <queue>
#include <semaphore>
#include <shared_mutex>
#include <thread>
class thread_pool {
  std::atomic<bool> can_submit{true};
  class worker_thread;                              
  std::atomic<std::size_t> max_task_count;         
  std::shared_mutex task_queue_mutex;             
  std::shared_mutex worker_list_mutex;            
  std::condition_variable_any task_queue_cv;      
  std::condition_variable_any task_queue_empty_cv;
  std::queue<std::function<void()>> task_queue;  
  std::list<worker_thread> worker_list;          
  thread_pool(const thread_pool&) = delete;
  thread_pool(thread_pool&&) = delete;
  thread_pool& operator=(const thread_pool&) = delete;
  thread_pool& operator=(thread_pool&&) = delete;

 public:
  thread_pool(std::size_t initial_thread_count, std::size_t max_task_count = 0);
  ~thread_pool();

  template <typename F, typename... Args>
  auto submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))>;
  void pause();
  void resume();
  void shutdown();
  void shutdown_now();
  void terminate();
  void wait();
  void add_thread(std::size_t count_to_add);
  void remove_thread(std::size_t count_to_remove);
  void set_max_task_count(std::size_t count_to_set);
  std::size_t get_thread_count();
  std::size_t get_task_count();
};
class thread_pool::worker_thread {
  std::binary_semaphore pause_sem;
  thread_pool* pool;             
  std::jthread thread{};         
  worker_thread(const worker_thread&) = delete;
  worker_thread(worker_thread&&) = delete;
  worker_thread& operator=(const worker_thread&) = delete;
  worker_thread& operator=(worker_thread&&) = delete;

 public:
  worker_thread(thread_pool* pool);
  ~worker_thread();
  void pause();
  void resume();
  void join();
};
inline thread_pool::thread_pool(const std::size_t initial_thread_count, const std::size_t max_task_count)
    : max_task_count(max_task_count) {
  if (max_task_count != 0 && initial_thread_count > max_task_count) {
    throw std::invalid_argument("max_task_count must be greater than initial_thread_count");
  }
  for (int i = 0; i < initial_thread_count; i++) {
    worker_list.emplace_back(this);
  }
}
inline thread_pool::~thread_pool() {
  can_submit = false;
  shutdown_now();
}
inline void thread_pool::pause() {
  can_submit = false;
  std::unique_lock lock(worker_list_mutex);
  for (auto& worker : worker_list) {
    worker.pause();
  }
}
inline void thread_pool::resume() {
  can_submit = true;
  std::unique_lock lock(worker_list_mutex);
  for (auto& worker : worker_list) {
    worker.resume();
  }
}
inline void thread_pool::shutdown() {
  can_submit = false;
  wait();
}
inline void thread_pool::shutdown_now() {
  can_submit = false;
  {
    std::unique_lock lk(task_queue_mutex);
    while (!task_queue.empty()) {
      task_queue.pop();
    }
  }
  wait();
}
inline void thread_pool::terminate() {
  can_submit = false;
  shutdown_now();
}
inline void thread_pool::wait() {
  std::shared_lock lk(task_queue_mutex);
  task_queue_empty_cv.wait(lk, [this] { return task_queue.empty(); });
  lk.unlock();
  std::unique_lock lk1(worker_list_mutex);
  for (auto& worker : worker_list) {
    worker.join();
  }
}
inline void thread_pool::add_thread(const std::size_t count_to_add) {
  if (max_task_count != 0 && count_to_add + worker_list.size() > max_task_count) {
    throw std::invalid_argument("max_task_count must be greater than initial_thread_count");
  }
  std::unique_lock lk(worker_list_mutex);
  for (std::size_t i = 0; i < count_to_add; i++) {
    worker_list.emplace_back(this);
  }
}
inline void thread_pool::remove_thread(const std::size_t count_to_remove) {
  std::unique_lock lk(worker_list_mutex);
  for (int i = 0; i < count_to_remove; i++) {
    worker_list.pop_back();
  }
}
inline void thread_pool::set_max_task_count(const std::size_t count_to_set) {
  std::unique_lock lk(task_queue_mutex);
  max_task_count = count_to_set;
}
inline std::size_t thread_pool::get_thread_count() {
  std::shared_lock lk(worker_list_mutex);
  return worker_list.size();
}
inline std::size_t thread_pool::get_task_count() {
  std::shared_lock lk(task_queue_mutex);
  return task_queue.size();
}

inline thread_pool::worker_thread::worker_thread(thread_pool* pool) : pause_sem(1), pool(pool) {
  thread = std::jthread([this](const std::stop_token& stop_token) {
    while (true) {
      using namespace std::chrono_literals;
      std::function<void()> task;
      {
        std::unique_lock lk(this->pool->task_queue_mutex);
        while (!this->pool->task_queue_cv.wait_for(lk, 1ms, [this] { return !this->pool->task_queue.empty(); })) {
          if (stop_token.stop_requested()) {
            return 0;
          }
        }
        pause_sem.acquire();
        pause_sem.release();
        task = this->pool->task_queue.front();
        this->pool->task_queue.pop();
        if (this->pool->task_queue.empty()) {
          this->pool->task_queue_empty_cv.notify_all();
        }
      }
      task();
      if (stop_token.stop_requested()) {
        return 0;
      }
    }
  });
}
inline thread_pool::worker_thread::~worker_thread() { thread.request_stop(); }
inline void thread_pool::worker_thread::pause() { pause_sem.acquire(); }
inline void thread_pool::worker_thread::resume() { pause_sem.release(); }
inline void thread_pool::worker_thread::join() {
  thread.request_stop();
  this->pool->task_queue_cv.notify_all();
  thread = std::jthread([this](const std::stop_token& stop_token) {
   while (true) {
     using namespace std::chrono_literals;
     std::function<void()> task;
     {
       std::unique_lock lk(this->pool->task_queue_mutex);
       while (!this->pool->task_queue_cv.wait_for(lk, 1ms, [this] { return !this->pool->task_queue.empty(); })) {
         if (stop_token.stop_requested()) {
           return 0;
         }
       }
       pause_sem.acquire();
       pause_sem.release();
       task = this->pool->task_queue.front();
       this->pool->task_queue.pop();
       if (this->pool->task_queue.empty()) {
         this->pool->task_queue_empty_cv.notify_all();
       }
     }
     task();
     if (stop_token.stop_requested()) {
       return 0;
     }
   }
 });
}
template <typename F, typename... Args>
auto thread_pool::submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
  if (!can_submit) {
    throw std::runtime_error("[thread_pool::submit][error]: thread pool is paused");
  }
  std::future<decltype(f(args...))> ret;
  auto task = std::make_shared<std::packaged_task<decltype(f(args...))()>>(
      std::bind(std::forward<F>(f), std::forward<Args>(args)...));
  ret = task->get_future();
  {
    std::unique_lock lk(task_queue_mutex);
    if (task_queue.size() >= max_task_count && max_task_count != 0) {
      throw std::runtime_error("[thread_pool::submit][error]: task queue is full");
    }
    task_queue.emplace([task] { (*task)(); });
  }
  task_queue_cv.notify_one();
  return ret;
}
#endif  // THREAD_POOL_H
