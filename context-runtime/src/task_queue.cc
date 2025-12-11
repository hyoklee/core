/**
 * TaskQueue implementation - helper functions for task queue operations
 */

#include "chimaera/task_queue.h"

namespace chi {

/**
 * Emplace a task into a task lane
 * @param lane_ptr Pointer to the task lane
 * @param task_ptr Pointer to the task to enqueue
 * @return true if successful, false otherwise
 */
bool TaskQueue_EmplaceTask(hipc::FullPtr<TaskLane>& lane_ptr, hipc::ShmPtr<Task> task_ptr) {
  if (lane_ptr.IsNull() || task_ptr.IsNull()) {
    return false;
  }

  // Push to the lane
  lane_ptr->Push(task_ptr);

  return true;
}

/**
 * Pop a task from a task lane
 * @param lane_ptr Pointer to the task lane
 * @param task_ptr Reference to store the popped task
 * @return true if a task was popped, false otherwise
 */
bool TaskQueue_PopTask(hipc::FullPtr<TaskLane>& lane_ptr, hipc::ShmPtr<Task>& task_ptr) {
  if (lane_ptr.IsNull()) {
    return false;
  }

  return lane_ptr->Pop(task_ptr);
}

/**
 * Pop a task from a task lane (overload for raw pointer)
 * @param lane_ptr Raw pointer to the task lane
 * @param task_ptr Reference to store the popped task
 * @return true if a task was popped, false otherwise
 */
bool TaskQueue_PopTask(TaskLane *lane_ptr, hipc::ShmPtr<Task>& task_ptr) {
  if (!lane_ptr) {
    return false;
  }

  return lane_ptr->Pop(task_ptr);
}

}  // namespace chi