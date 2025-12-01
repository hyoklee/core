/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Distributed under BSD 3-Clause license.                                   *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Illinois Institute of Technology.                        *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of Hermes. The full Hermes copyright notice, including  *
 * terms governing use, modification, and redistribution, is contained in    *
 * the COPYING file, which can be found at the top directory. If you do not  *
 * have access to the file, you may request a copy from help@hdfgroup.org.   *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef HSHM_INCLUDE_MEMORY_BACKEND_POSIX_SHM_MMAP_H
#define HSHM_INCLUDE_MEMORY_BACKEND_POSIX_SHM_MMAP_H

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "hermes_shm/constants/macros.h"
#include "hermes_shm/introspect/system_info.h"
#include "hermes_shm/util/errors.h"
#include "hermes_shm/util/logging.h"
#include "memory_backend.h"

namespace hshm::ipc {

class PosixShmMmap : public MemoryBackend, public UrlMemoryBackend {
 protected:
  File fd_;
  std::string url_;
  size_t total_size_;

 public:
  /** Constructor */
  HSHM_CROSS_FUN
  PosixShmMmap() {}

  /** Destructor */
  HSHM_CROSS_FUN
  ~PosixShmMmap() {
#if HSHM_IS_HOST
    if (IsOwned()) {
      _Destroy();
    } else {
      _Detach();
    }
#endif
  }

  /**
   * Initialize backend with mixed private/shared mapping
   *
   * Creates a contiguous virtual memory region:
   * - First kBackendPrivate (16KB): PRIVATE mapping (process-local, not shared)
   * - Remaining: SHARED mapping (inter-process shared memory)
   *
   * The private region can be used for process-local metadata, TLS pointers,
   * or other data that should not be shared between processes.
   *
   * @param backend_id Unique identifier for this backend
   * @param size Size of the shared data region (excluding kBackendPrivate private region)
   * @param url POSIX shared memory object name (e.g., "/my_shm")
   * @return true on success, false on failure
   *
   * Memory layout: [kBackendPrivate private] [MemoryBackendHeader] [data]
   *                 ^ptr                      ^header_             ^data_
   */
  bool shm_init(const MemoryBackendId &backend_id, size_t size,
                const std::string &url) {
    // Enforce minimum backend size of 1MB
    constexpr size_t kMinBackendSize = 1024 * 1024;  // 1MB
    if (size < kMinBackendSize) {
      size = kMinBackendSize;
    }

    // Initialize flags before calling methods that use it
    flags_.Clear();
    SetInitialized();
    Own();

    // Calculate sizes: header + md section + alignment + data section
    constexpr size_t kAlignment = 4096;  // 4KB alignment
    size_t header_size = sizeof(MemoryBackendHeader);
    size_t md_size = header_size;
    size_t aligned_md_size = ((md_size + kAlignment - 1) / kAlignment) * kAlignment;

    // Total layout: [kBackendPrivate private | shared_size]
    size_t shared_size = aligned_md_size + size;
    total_size_ = kBackendPrivate + shared_size;

    // Create shared memory object (only for the shared portion)
    SystemInfo::DestroySharedMemory(url);
    if (!SystemInfo::CreateNewSharedMemory(fd_, url, shared_size)) {
      char *err_buf = strerror(errno);
      HILOG(kError, "shm_open failed: {}", err_buf);
      return false;
    }
    url_ = url;

    // Create mixed mapping: [kBackendPrivate private | shared_size shared]
    char *ptr = reinterpret_cast<char *>(
        SystemInfo::MapMixedMemory(fd_, kBackendPrivate, shared_size, 0));
    if (!ptr) {
      HILOG(kError, "Failed to create mixed mapping");
      SystemInfo::CloseSharedMemory(fd_);
      return false;
    }

    char *shared_ptr = ptr + kBackendPrivate;

    // Now we have: [kBackendPrivate private | shared_size bytes shared]
    // The first kBackendPrivate is private (process-local), rest is shared

    // Layout: [kBackendPrivate private] [MemoryBackendHeader | padding to 4KB] [data]
    header_ = reinterpret_cast<MemoryBackendHeader *>(shared_ptr);
    new (header_) MemoryBackendHeader();
    header_->id_ = backend_id;
    header_->md_size_ = md_size;
    header_->data_size_ = size;
    header_->data_id_ = -1;
    header_->flags_.Clear();

    // md_ points to the header itself (metadata for process connection)
    md_ = shared_ptr;
    md_size_ = md_size;

    // data_ starts at 4KB aligned boundary after md section
    data_ = shared_ptr + aligned_md_size;
    data_size_ = size;
    data_id_ = -1;
    data_offset_ = 0;

    return true;
  }

  /**
   * Attach to existing backend with mixed private/shared mapping
   *
   * Recreates the same memory layout as the initializing process:
   * - First kBackendPrivate (16KB): PRIVATE mapping (process-local, independent per process)
   * - Remaining: SHARED mapping (attached to shared memory object)
   *
   * @param url POSIX shared memory object name
   * @return true on success, false on failure
   */
  bool shm_attach(const std::string &url) {
    flags_.Clear();
    SetInitialized();
    Disown();

    if (!SystemInfo::OpenSharedMemory(fd_, url)) {
      const char *err_buf = strerror(errno);
      HILOG(kError, "shm_open failed: {}", err_buf);
      return false;
    }
    url_ = url;

    // Map just the header temporarily to read size information
    constexpr size_t kAlignment = 4096;
    char *temp_header = reinterpret_cast<char *>(
        SystemInfo::MapSharedMemory(fd_, kAlignment, 0));
    if (!temp_header) {
      HILOG(kError, "Failed to map header");
      SystemInfo::CloseSharedMemory(fd_);
      return false;
    }

    // Read header information
    MemoryBackendHeader *hdr = reinterpret_cast<MemoryBackendHeader *>(temp_header);
    size_t md_size = hdr->md_size_;
    size_t data_size = hdr->data_size_;

    // Calculate sizes
    size_t aligned_md_size = ((md_size + kAlignment - 1) / kAlignment) * kAlignment;
    size_t shared_size = aligned_md_size + data_size;
    total_size_ = kBackendPrivate + shared_size;

    // Unmap the temporary header
    SystemInfo::UnmapMemory(temp_header, kAlignment);

    // Create mixed mapping with correct sizes
    char *ptr = reinterpret_cast<char *>(
        SystemInfo::MapMixedMemory(fd_, kBackendPrivate, shared_size, 0));
    if (!ptr) {
      HILOG(kError, "Failed to create mixed mapping during attach");
      SystemInfo::CloseSharedMemory(fd_);
      return false;
    }

    char *shared_ptr = ptr + kBackendPrivate;

    // Set up pointers (same layout as shm_init)
    header_ = reinterpret_cast<MemoryBackendHeader *>(shared_ptr);
    md_ = shared_ptr;
    md_size_ = md_size;
    data_ = shared_ptr + aligned_md_size;
    data_size_ = data_size;
    data_id_ = header_->data_id_;
    data_offset_ = 0;

    return true;
  }

  /** Detach the mapped memory */
  void shm_detach() { _Detach(); }

  /** Destroy the mapped memory */
  void shm_destroy() { _Destroy(); }

  // GetPrivateRegion() and GetPrivateRegionSize() are inherited from MemoryBackend base class

 protected:
  /** Map shared memory */
  char *_ShmMap(size_t size, i64 off) {
    char *ptr =
        reinterpret_cast<char *>(SystemInfo::MapSharedMemory(fd_, size, off));
    if (!ptr) {
      HSHM_THROW_ERROR(SHMEM_CREATE_FAILED);
    }
    return ptr;
  }

  /** Unmap shared memory */
  void _Detach() {
    if (!IsInitialized()) {
      return;
    }
    // Unmap the entire contiguous region
    SystemInfo::UnmapMemory(reinterpret_cast<void *>(header_), total_size_);
    SystemInfo::CloseSharedMemory(fd_);
    UnsetInitialized();
  }

  /** Destroy shared memory */
  void _Destroy() {
    if (!IsInitialized()) {
      return;
    }
    _Detach();
    SystemInfo::DestroySharedMemory(url_);
    UnsetInitialized();
  }
};

}  // namespace hshm::ipc

#endif  // HSHM_INCLUDE_MEMORY_BACKEND_POSIX_SHM_MMAP_H
