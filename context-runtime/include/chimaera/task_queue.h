#ifndef CHIMAERA_INCLUDE_CHIMAERA_TASK_QUEUE_H_
#define CHIMAERA_INCLUDE_CHIMAERA_TASK_QUEUE_H_

#include <atomic>
#include <sys/types.h>
#include "chimaera/types.h"
#include "chimaera/future.h"

namespace chi {

/**
 * Custom header for tracking lane state (stored per-lane)
 */
struct TaskQueueHeader {
  PoolId pool_id;
  WorkerId assigned_worker_id;
  u32 task_count;        // Number of tasks currently in the queue
  bool is_enqueued;      // Whether this queue is currently enqueued in worker
  int signal_fd_;        // Signal file descriptor for awakening worker
  pid_t tid_;            // Thread ID of the worker owning this lane
  std::atomic<bool> active_;  // Whether worker is accepting tasks (true) or blocked in epoll_wait (false)

  TaskQueueHeader()
    : pool_id(), assigned_worker_id(0), task_count(0), is_enqueued(false),
      signal_fd_(-1), tid_(0) {
    active_.store(true);
  }

  TaskQueueHeader(PoolId pid, WorkerId wid = 0)
    : pool_id(pid), assigned_worker_id(wid), task_count(0), is_enqueued(false),
      signal_fd_(-1), tid_(0) {
    active_.store(true);
  }
};

// Type alias for individual lanes with per-lane headers (moved outside TaskQueue class)
// Worker queues store Future<Task> objects directly
using TaskLane =
    hipc::multi_mpsc_ring_buffer<Future<Task>,
                                 CHI_MAIN_ALLOC_T>::ring_buffer_type;

/**
 * Simple wrapper around hipc::multi_mpsc_ring_buffer
 *
 * This wrapper adds custom enqueue and dequeue functions while maintaining
 * compatibility with existing code that expects the multi_mpsc_ring_buffer
 * interface.
 */
typedef hipc::multi_mpsc_ring_buffer<Future<Task>, CHI_MAIN_ALLOC_T>
    TaskQueue;

} // namespace chi

#endif // CHIMAERA_INCLUDE_CHIMAERA_TASK_QUEUE_H_