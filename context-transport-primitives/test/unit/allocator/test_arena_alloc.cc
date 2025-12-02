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

#include <catch2/catch_test_macros.hpp>
#include "allocator_test.h"
#include "hermes_shm/memory/backend/malloc_backend.h"
#include "hermes_shm/memory/allocator/arena_allocator.h"

using hshm::testing::AllocatorTest;

TEST_CASE("ArenaAllocator - Basic Allocation", "[ArenaAllocator]") {
  hipc::MallocBackend backend;
  size_t arena_size = 1024 * 1024;  // 1 MB
  size_t alloc_size = sizeof(hipc::ArenaAllocator<false>);
  backend.shm_init(hipc::MemoryBackendId(0, 0), alloc_size + arena_size);

  // MakeAlloc constructs the allocator in the backend and initializes it
  auto *alloc = backend.MakeAlloc<hipc::ArenaAllocator<false>>();

  SECTION("Single allocation") {
    size_t alloc_header_size = sizeof(hipc::ArenaAllocator<false>);
    auto ptr = alloc->AllocateOffset( 100);
    REQUIRE_FALSE(ptr.IsNull());
    REQUIRE(ptr.off_.load() == alloc_header_size);  // First allocation after allocator header
    REQUIRE(alloc->GetHeapOffset() == alloc_header_size + 100);
  }

  SECTION("Multiple allocations") {
    size_t alloc_header_size = sizeof(hipc::ArenaAllocator<false>);
    auto ptr1 = alloc->AllocateOffset( 100);
    auto ptr2 = alloc->AllocateOffset( 200);
    auto ptr3 = alloc->AllocateOffset( 300);

    REQUIRE(ptr1.off_.load() == alloc_header_size);
    REQUIRE(ptr2.off_.load() == alloc_header_size + 100);
    REQUIRE(ptr3.off_.load() == alloc_header_size + 300);
    REQUIRE(alloc->GetHeapOffset() == alloc_header_size + 600);
  }

  // Note: Allocation tracking requires HSHM_ALLOC_TRACK_SIZE to be defined

  backend.shm_destroy();
}

TEST_CASE("ArenaAllocator - Aligned Allocation", "[ArenaAllocator]") {
  hipc::MallocBackend backend;
  size_t arena_size = 1024 * 1024;
  size_t alloc_size = sizeof(hipc::ArenaAllocator<false>);
  backend.shm_init(hipc::MemoryBackendId(0, 0), alloc_size + arena_size);

  // MakeAlloc constructs the allocator in the backend and initializes it
  auto *alloc = backend.MakeAlloc<hipc::ArenaAllocator<false>>();

  SECTION("Aligned allocations") {
    

    // Allocate 100 bytes aligned to 64
    auto ptr1 = alloc->AllocateOffset( 100, 64);
    REQUIRE(ptr1.off_.load() % 64 == 0);

    // Next allocation should also be 64-byte aligned
    auto ptr2 = alloc->AllocateOffset( 50, 64);
    REQUIRE(ptr2.off_.load() % 64 == 0);
  }

  SECTION("Mixed alignment") {
    size_t alloc_header_size = sizeof(hipc::ArenaAllocator<false>);
    auto ptr1 = alloc->AllocateOffset( 1);  // 1 byte
    auto ptr2 = alloc->AllocateOffset( 1, 64);  // Align to 64

    REQUIRE(ptr1.off_.load() == alloc_header_size);
    // After 1 byte at alloc_header_size, next 64-byte boundary depends on header size
    // If header is 120 bytes (multiple of 8), next allocation at 120+1=121, then align to next 64-byte boundary
    size_t expected_aligned = ((alloc_header_size + 1 + 63) / 64) * 64;
    REQUIRE(ptr2.off_.load() == expected_aligned);
  }

  backend.shm_destroy();
}

TEST_CASE("ArenaAllocator - Reset", "[ArenaAllocator]") {
  hipc::MallocBackend backend;
  size_t arena_size = 1024 * 1024;
  size_t alloc_size = sizeof(hipc::ArenaAllocator<false>);
  backend.shm_init(hipc::MemoryBackendId(0, 0), alloc_size + arena_size);

  // MakeAlloc constructs the allocator in the backend and initializes it
  auto *alloc = backend.MakeAlloc<hipc::ArenaAllocator<false>>();

  size_t alloc_header_size = sizeof(hipc::ArenaAllocator<false>);

  // Allocate some memory
  alloc->AllocateOffset( 100);
  alloc->AllocateOffset( 200);
  alloc->AllocateOffset( 300);

  REQUIRE(alloc->GetHeapOffset() == alloc_header_size + 600);

  // Reset the arena
  alloc->Reset();

  REQUIRE(alloc->GetHeapOffset() == alloc_header_size);

  // Allocate again - should start from after header
  auto ptr = alloc->AllocateOffset( 50);
  REQUIRE(ptr.off_.load() == alloc_header_size);
  REQUIRE(alloc->GetHeapOffset() == alloc_header_size + 50);

  backend.shm_destroy();
}

TEST_CASE("ArenaAllocator - Out of Memory", "[ArenaAllocator]") {
  hipc::MallocBackend backend;
  // Use 2MB to avoid MallocBackend's 1MB minimum
  size_t arena_size = 2 * 1024 * 1024;
  size_t alloc_size = sizeof(hipc::ArenaAllocator<false>);
  backend.shm_init(hipc::MemoryBackendId(0, 0), alloc_size + arena_size);

  // MakeAlloc constructs the allocator in the backend and initializes it
  auto *alloc = backend.MakeAlloc<hipc::ArenaAllocator<false>>();

  // Get the actual remaining size
  size_t remaining = alloc->GetRemainingSize();

  // Allocate almost all available space
  alloc->AllocateOffset(remaining - 200);

  // This allocation should succeed
  REQUIRE_NOTHROW(alloc->AllocateOffset(100));

  // This allocation should fail - not enough space left
  REQUIRE_THROWS(alloc->AllocateOffset(200));

  backend.shm_destroy();
}

TEST_CASE("ArenaAllocator - Free is No-op", "[ArenaAllocator]") {
  hipc::MallocBackend backend;
  size_t arena_size = 1024 * 1024;
  size_t alloc_size = sizeof(hipc::ArenaAllocator<false>);
  backend.shm_init(hipc::MemoryBackendId(0, 0), alloc_size + arena_size);

  // MakeAlloc constructs the allocator in the backend and initializes it
  auto *alloc = backend.MakeAlloc<hipc::ArenaAllocator<false>>();

  

  auto ptr1 = alloc->Allocate<int>( 10);
  auto ptr2 = alloc->Allocate<int>( 20);

  size_t heap_before = alloc->GetHeapOffset();

  // Free should be a no-op
  REQUIRE_NOTHROW(alloc->Free( ptr1));
  REQUIRE_NOTHROW(alloc->Free( ptr2));

  // Heap offset should not change
  REQUIRE(alloc->GetHeapOffset() == heap_before);

  backend.shm_destroy();
}

TEST_CASE("ArenaAllocator - Remaining Space", "[ArenaAllocator]") {
  hipc::MallocBackend backend;
  // Use 2MB to avoid MallocBackend's 1MB minimum affecting the test
  size_t test_arena_size = 2 * 1024 * 1024;
  size_t alloc_size = sizeof(hipc::ArenaAllocator<false>);
  backend.shm_init(hipc::MemoryBackendId(0, 0), alloc_size + test_arena_size);

  // MakeAlloc constructs the allocator in the backend and initializes it
  auto *alloc = backend.MakeAlloc<hipc::ArenaAllocator<false>>();

  // GetRemainingSize returns the size of the managed heap region (after allocator header)
  size_t initial_remaining = alloc->GetRemainingSize();
  REQUIRE(initial_remaining >= test_arena_size);  // May be rounded up by backend

  alloc->AllocateOffset( 300);
  REQUIRE(alloc->GetRemainingSize() == initial_remaining - 300);

  alloc->AllocateOffset( 200);
  REQUIRE(alloc->GetRemainingSize() == initial_remaining - 500);

  alloc->Reset();
  REQUIRE(alloc->GetRemainingSize() == initial_remaining);

  backend.shm_destroy();
}

TEST_CASE("ArenaAllocator - Atomic Version", "[ArenaAllocator][atomic]") {
  hipc::MallocBackend backend;
  size_t arena_size = 1024 * 1024;
  size_t alloc_size = sizeof(hipc::ArenaAllocator<true>);
  backend.shm_init(hipc::MemoryBackendId(0, 0), alloc_size + arena_size);

  // MakeAlloc constructs the allocator in the backend and initializes it
  auto *alloc = backend.MakeAlloc<hipc::ArenaAllocator<true>>();

  SECTION("Basic atomic allocations") {
    size_t alloc_header_size = sizeof(hipc::ArenaAllocator<true>);
    auto ptr1 = alloc->AllocateOffset( 100);
    auto ptr2 = alloc->AllocateOffset( 200);

    REQUIRE(ptr1.off_.load() == alloc_header_size);
    REQUIRE(ptr2.off_.load() == alloc_header_size + 100);
    REQUIRE(alloc->GetHeapOffset() == alloc_header_size + 300);
  }

  SECTION("Atomic reset") {
    size_t alloc_header_size = sizeof(hipc::ArenaAllocator<true>);
    alloc->AllocateOffset( 500);
    REQUIRE(alloc->GetHeapOffset() == alloc_header_size + 500);

    alloc->Reset();
    REQUIRE(alloc->GetHeapOffset() == alloc_header_size);
  }

  backend.shm_destroy();
}

// Note: Type allocation tests are skipped because ArenaAllocator with MallocBackend
// doesn't provide a real memory buffer (MallocBackend has data_=nullptr).
// ArenaAllocator is designed to work with backends that provide actual buffers
// (like PosixShmMmap or ArrayBackend from sub-allocators).
