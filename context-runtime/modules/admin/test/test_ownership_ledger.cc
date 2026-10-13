/*
 * Copyright (c) 2026, The IOWarp Authors.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "simple_test.h"

#include <clio_runtime/admin/ownership_ledger.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace clio::run::admin {
namespace {

/** Return whether the callable throws the required exception type. */
template <typename Exception, typename Callable>
bool Throws(Callable &&callable) {
  try {
    std::forward<Callable>(callable)();
  } catch (const Exception &) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

/** Own a private directory on the persistent filesystem selected by CI. */
class LedgerDirectory {
 public:
  /** Create a distinct directory below CLIO_TEST_DATA_DIR or the working dir. */
  LedgerDirectory() {
    static std::atomic<unsigned int> sequence{0};
    const char *configured = std::getenv("CLIO_TEST_DATA_DIR");
    const auto base = configured != nullptr && configured[0] != '\0'
                          ? std::filesystem::absolute(configured)
                          : std::filesystem::current_path();
#if defined(_WIN32)
    const auto process = _getpid();
#else
    const auto process = getpid();
#endif
    const auto epoch = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = base / ("ownership-ledger-" + std::to_string(process) + "-" +
                    std::to_string(epoch) + "-" +
                    std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
    std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
  }

  /** Remove only the private directory created by this instance. */
  ~LedgerDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  /** Return the absolute path of the authority ledger. */
  std::string file() const { return (path_ / "authority.ledger").string(); }

  LedgerDirectory(const LedgerDirectory &) = delete;
  LedgerDirectory &operator=(const LedgerDirectory &) = delete;

 private:
  std::filesystem::path path_;
};

#if defined(__linux__)

/** Produce a valid, distinct 64-character lower-case ownership nonce. */
std::string Nonce(unsigned int value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string nonce(64, '0');
  for (std::size_t offset = 0; value != 0; ++offset) {
    nonce[nonce.size() - 1 - offset] = digits[value & 0xfU];
    value >>= 4U;
  }
  return nonce;
}

/** Read exact ledger bytes without attempting recovery or interpretation. */
std::string ReadBytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("Cannot open ledger test input");
  }
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/** Replace a test ledger with exact bytes to exercise fail-closed recovery. */
void WriteBytes(const std::filesystem::path &path, const std::string &bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  if (!output) {
    throw std::runtime_error("Cannot write ledger test input");
  }
}

/** Append an unsigned integer in the documented little-endian wire format. */
void AppendInteger(std::string &bytes, uint64_t value, unsigned int width) {
  for (unsigned int index = 0; index < width; ++index) {
    bytes.push_back(static_cast<char>((value >> (8U * index)) & 0xffU));
  }
}

/** Append one documented length-prefixed string to a deliberately built frame. */
void AppendString(std::string &bytes, const std::string &value) {
  AppendInteger(bytes, value.size(), 4);
  bytes.append(value);
}

/** Calculate IEEE CRC32 so malformed state transitions retain valid checksums. */
uint32_t FrameChecksum(const std::string &payload) {
  uint32_t checksum = 0xffffffffU;
  for (const auto byte : payload) {
    checksum ^= static_cast<unsigned char>(byte);
    for (unsigned int bit = 0; bit < 8; ++bit) {
      checksum = (checksum >> 1U) ^ ((checksum & 1U) != 0 ? 0xedb88320U : 0U);
    }
  }
  return ~checksum;
}

/** Build a checksum-valid transition solely to test replay rejection boundaries. */
std::string TransitionFrame(char state, uint64_t revision,
                            const std::string &resource,
                            const std::string &nonce = "",
                            const std::string &label = "",
                            const std::string &metadata = "") {
  std::string payload(1, state);
  AppendInteger(payload, revision, 8);
  AppendString(payload, resource);
  AppendString(payload, nonce);
  AppendString(payload, label);
  AppendString(payload, metadata);
  std::string frame;
  AppendInteger(frame, payload.size(), 4);
  AppendInteger(frame, FrameChecksum(payload), 4);
  frame.append(payload);
  return frame;
}

/** Reap a child and require a clean, explicitly successful exit. */
void RequireChildSuccess(pid_t child) {
  int status = 0;
  REQUIRE(waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
}

/** Check that a failed constructor did not retain the exclusive file lock. */
void RequireUnlocked(const std::filesystem::path &path) {
  const int descriptor = open(path.c_str(), O_RDWR | O_CLOEXEC);
  REQUIRE(descriptor >= 0);
  const int locked = flock(descriptor, LOCK_EX | LOCK_NB);
  if (locked == 0) {
    flock(descriptor, LOCK_UN);
  }
  close(descriptor);
  REQUIRE(locked == 0);
}
#endif

}  // namespace

#if !defined(__linux__)

TEST_CASE("Ownership authority fails closed on unsupported non-Linux storage",
          "[admin][ownership]") {
  LedgerDirectory directory;
  REQUIRE(Throws<std::runtime_error>([&] {
    OwnershipLedger ledger(directory.file());
  }));
  REQUIRE_FALSE(std::filesystem::exists(directory.file()));
}

#else

TEST_CASE("Ownership acquisition has exactly one concurrent winner",
          "[admin][ownership]") {
  LedgerDirectory directory;
  OwnershipLedger ledger(directory.file());
  const auto authority = ledger.Inspect("blueprints").record.authority_uuid;
  constexpr std::size_t count = 8;
  std::array<std::string, count> outcomes;
  std::array<std::exception_ptr, count> errors{};
  std::vector<std::thread> workers;
  std::mutex mutex;
  std::condition_variable ready_condition;
  std::condition_variable start_condition;
  std::size_t ready = 0;
  bool start = false;
  for (std::size_t index = 0; index < count; ++index) {
    workers.emplace_back([&, index] {
      {
        std::unique_lock<std::mutex> lock(mutex);
        ++ready;
        ready_condition.notify_one();
        start_condition.wait(lock, [&] { return start; });
      }
      try {
        outcomes[index] =
            ledger.Acquire("blueprints", Nonce(static_cast<unsigned int>(index + 1)),
                           "writer-" + std::to_string(index), authority,
                           "thread-" + std::to_string(index))
                .outcome;
      } catch (...) {
        errors[index] = std::current_exception();
      }
    });
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    ready_condition.wait(lock, [&] { return ready == count; });
    start = true;
  }
  start_condition.notify_all();
  for (auto &worker : workers) {
    worker.join();
  }
  for (const auto &error : errors) {
    REQUIRE(error == nullptr);
  }
  REQUIRE(std::count(outcomes.begin(), outcomes.end(), "acquired") == 1);
  REQUIRE(std::count(outcomes.begin(), outcomes.end(), "busy") == count - 1);
  const auto current = ledger.Inspect("blueprints");
  REQUIRE(current.outcome == "inspected");
  REQUIRE(current.record.held);
  REQUIRE(current.record.revision == 1);
}

TEST_CASE("Ownership retries and busy claims do not append or change the holder",
          "[admin][ownership]") {
  LedgerDirectory directory;
  OwnershipLedger ledger(directory.file());
  const auto authority = ledger.Inspect("blueprints").record.authority_uuid;
  const auto acquired =
      ledger.Acquire("blueprints", Nonce(1), "first", authority, "original metadata");
  REQUIRE(acquired.outcome == "acquired");
  const auto bytes = ReadBytes(directory.file());
  const auto retry =
      ledger.Acquire("blueprints", Nonce(1), "renamed", authority, "different metadata");
  REQUIRE(retry.outcome == "acquired");
  REQUIRE(retry.record.revision == acquired.record.revision);
  REQUIRE(retry.record.owner_label == "first");
  REQUIRE(retry.record.holder_metadata == "original metadata");
  REQUIRE(ReadBytes(directory.file()) == bytes);
  const auto busy = ledger.Acquire("blueprints", Nonce(2), "second", authority, "other");
  REQUIRE(busy.outcome == "busy");
  REQUIRE(busy.record.revision == acquired.record.revision);
  REQUIRE(busy.record.owner_label == "first");
  REQUIRE(busy.record.holder_metadata == "original metadata");
  REQUIRE(ReadBytes(directory.file()) == bytes);
}

TEST_CASE("Ownership rejects mismatched authority and release tokens without effects",
          "[admin][ownership]") {
  LedgerDirectory directory;
  OwnershipLedger ledger(directory.file());
  const auto authority = ledger.Inspect("blueprints").record.authority_uuid;
  const auto acquired = ledger.Acquire("blueprints", Nonce(1), "first", authority, "");
  const auto revision = acquired.record.revision;
  const auto bytes = ReadBytes(directory.file());
  REQUIRE(Throws<std::invalid_argument>([&] {
    ledger.Acquire("blueprints", Nonce(2), "second", "wrong-authority", "");
  }));
  REQUIRE(Throws<std::invalid_argument>([&] {
    ledger.Release("blueprints", "wrong-authority", revision, Nonce(1));
  }));
  REQUIRE(ledger.Release("blueprints", authority, revision, Nonce(2)).outcome ==
          "rejected");
  REQUIRE(ledger.Release("blueprints", authority, revision + 1, Nonce(1)).outcome ==
          "rejected");
  REQUIRE(ledger.Release("other", authority, revision, Nonce(1)).outcome == "rejected");
  const auto current = ledger.Inspect("blueprints").record;
  REQUIRE(current.held);
  REQUIRE(current.revision == revision);
  REQUIRE(current.owner_label == "first");
  REQUIRE(ReadBytes(directory.file()) == bytes);
}

TEST_CASE("Ownership tombstones fence old owners and use global transition revisions",
          "[admin][ownership]") {
  LedgerDirectory directory;
  OwnershipLedger ledger(directory.file());
  const auto authority = ledger.Inspect("blueprints").record.authority_uuid;
  const auto first = ledger.Acquire("blueprints", Nonce(1), "first", authority, "");
  REQUIRE(first.record.revision == 1);
  const auto other = ledger.Acquire("other-store", Nonce(2), "other", authority, "");
  REQUIRE(other.record.revision == 2);
  const auto released =
      ledger.Release("blueprints", authority, first.record.revision, Nonce(1));
  REQUIRE(released.outcome == "released");
  REQUIRE_FALSE(released.record.held);
  REQUIRE(released.record.revision == 3);
  const auto second = ledger.Acquire("blueprints", Nonce(3), "second", authority, "");
  REQUIRE(second.outcome == "acquired");
  REQUIRE(second.record.revision == 4);
  const auto bytes = ReadBytes(directory.file());
  REQUIRE(ledger.Release("blueprints", authority, first.record.revision, Nonce(1))
              .outcome == "rejected");
  REQUIRE(ledger.Release("blueprints", authority, second.record.revision, Nonce(1))
              .outcome == "rejected");
  REQUIRE(ledger.Inspect("blueprints").record.owner_label == "second");
  REQUIRE(ReadBytes(directory.file()) == bytes);
}

TEST_CASE("Ownership restart retains authority held claims and free tombstones",
          "[admin][ownership]") {
  LedgerDirectory directory;
  std::string authority;
  uint64_t held_revision = 0;
  uint64_t free_revision = 0;
  {
    OwnershipLedger ledger(directory.file());
    authority = ledger.Inspect("held").record.authority_uuid;
    const auto held = ledger.Acquire("held", Nonce(1), "persistent", authority, "host A");
    held_revision = held.record.revision;
    const auto temporary = ledger.Acquire("free", Nonce(2), "temporary", authority, "");
    free_revision = ledger.Release("free", authority, temporary.record.revision, Nonce(2))
                        .record.revision;
  }
  {
    OwnershipLedger ledger(directory.file());
    const auto held = ledger.Inspect("held").record;
    REQUIRE(held.authority_uuid == authority);
    REQUIRE(held.held);
    REQUIRE(held.revision == held_revision);
    REQUIRE(held.owner_label == "persistent");
    REQUIRE(held.holder_metadata == "host A");
    REQUIRE(ledger.Acquire("held", Nonce(3), "new process", authority, "").outcome ==
            "busy");
    const auto free = ledger.Inspect("free").record;
    REQUIRE(free.authority_uuid == authority);
    REQUIRE_FALSE(free.held);
    REQUIRE(free.revision == free_revision);
    const auto next = ledger.Acquire("free", Nonce(4), "successor", authority, "");
    REQUIRE(next.record.revision == free_revision + 1);
  }
}

TEST_CASE("Ownership does not release a durable claim when its client process dies",
          "[admin][ownership]") {
  LedgerDirectory directory;
  std::string authority;
  {
    OwnershipLedger ledger(directory.file());
    authority = ledger.Inspect("blueprints").record.authority_uuid;
  }
  const pid_t child = fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    try {
      OwnershipLedger ledger(directory.file());
      const auto acquired =
          ledger.Acquire("blueprints", Nonce(1), "departed client", authority, "child");
      _exit(acquired.outcome == "acquired" ? 0 : 1);
    } catch (...) {
      _exit(2);
    }
  }
  RequireChildSuccess(child);
  OwnershipLedger ledger(directory.file());
  const auto held = ledger.Inspect("blueprints").record;
  REQUIRE(held.held);
  REQUIRE(held.owner_label == "departed client");
  REQUIRE(held.revision == 1);
  REQUIRE(ledger.Acquire("blueprints", Nonce(2), "successor", authority, "").outcome ==
          "busy");
}

TEST_CASE("Ownership permits only one writer process for an authority ledger",
          "[admin][ownership]") {
  LedgerDirectory directory;
  OwnershipLedger ledger(directory.file());
  const auto authority = ledger.Inspect("blueprints").record.authority_uuid;
  REQUIRE_FALSE(authority.empty());
  const pid_t child = fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    const bool denied = Throws<std::runtime_error>([&] {
      OwnershipLedger duplicate(directory.file());
    });
    _exit(denied ? 0 : 1);
  }
  RequireChildSuccess(child);
  REQUIRE(ledger.Acquire("blueprints", Nonce(1), "original", authority, "").outcome ==
          "acquired");
}

TEST_CASE("Ownership refuses existing empty corrupt and torn ledgers without repair",
          "[admin][ownership]") {
  LedgerDirectory directory;
  {
    OwnershipLedger ledger(directory.file());
    const auto authority = ledger.Inspect("blueprints").record.authority_uuid;
    REQUIRE(ledger.Acquire("blueprints", Nonce(1), "first", authority, "metadata")
                .outcome == "acquired");
  }
  const auto original = ReadBytes(directory.file());
  REQUIRE(original.size() > 8);
  auto corrupt = original;
  corrupt.back() ^= 0x01;
  const std::array<std::string, 4> invalid = {
      std::string{}, original.substr(0, 7), corrupt,
      original.substr(0, original.size() - 1)};
  for (const auto &bytes : invalid) {
    INFO("Rejected ledger byte length: " + std::to_string(bytes.size()));
    WriteBytes(directory.file(), bytes);
    REQUIRE(Throws<std::runtime_error>([&] {
      OwnershipLedger ledger(directory.file());
    }));
    REQUIRE(ReadBytes(directory.file()) == bytes);
    RequireUnlocked(directory.file());
  }
}

TEST_CASE("Ownership validates resource nonce label and metadata bounds",
          "[admin][ownership]") {
  LedgerDirectory directory;
  OwnershipLedger ledger(directory.file());
  const auto authority = ledger.Inspect("blueprints").record.authority_uuid;
  const auto before = ReadBytes(directory.file());
  const std::array<std::string, 3> bad_resources = {
      std::string{}, std::string(1025, 'r'), std::string("bad\0resource", 12)};
  for (const auto &resource : bad_resources) {
    REQUIRE(Throws<std::invalid_argument>([&] { ledger.Inspect(resource); }));
    REQUIRE(Throws<std::invalid_argument>([&] {
      ledger.Acquire(resource, Nonce(1), "first", authority, "");
    }));
  }
  const std::array<std::string, 5> bad_nonces = {
      std::string{}, std::string(63, 'a'), std::string(65, 'a'),
      std::string(64, 'A'), std::string(64, 'g')};
  for (const auto &nonce : bad_nonces) {
    REQUIRE(Throws<std::invalid_argument>([&] {
      ledger.Acquire("blueprints", nonce, "first", authority, "");
    }));
    REQUIRE(Throws<std::invalid_argument>([&] {
      ledger.Release("blueprints", authority, 0, nonce);
    }));
  }
  for (const auto &label : {std::string{}, std::string(257, 'l')}) {
    REQUIRE(Throws<std::invalid_argument>([&] {
      ledger.Acquire("blueprints", Nonce(1), label, authority, "");
    }));
  }
  REQUIRE(Throws<std::invalid_argument>([&] {
    ledger.Acquire("blueprints", Nonce(1), "first", authority, std::string(8193, 'm'));
  }));
  REQUIRE(ReadBytes(directory.file()) == before);
  const auto maximum = ledger.Acquire(std::string(1024, 'r'), Nonce(1),
                                      std::string(256, 'l'), authority,
                                      std::string(8192, 'm'));
  REQUIRE(maximum.outcome == "acquired");
  REQUIRE(maximum.record.owner_label.size() == 256);
  REQUIRE(maximum.record.holder_metadata.size() == 8192);
}

TEST_CASE("Ownership replay rejects checksum-valid illegal revisions and transitions",
          "[admin][ownership]") {
  LedgerDirectory directory;
  std::string header;
  std::string held;
  std::string released;
  {
    OwnershipLedger ledger(directory.file());
    const auto authority = ledger.Inspect("blueprints").record.authority_uuid;
    header = ReadBytes(directory.file());
    const auto first = ledger.Acquire("blueprints", Nonce(1), "first", authority, "");
    held = ReadBytes(directory.file());
    REQUIRE(ledger.Release("blueprints", authority, first.record.revision, Nonce(1))
                .outcome == "released");
    released = ReadBytes(directory.file());
  }
  const std::array<std::string, 5> invalid = {
      header + TransitionFrame('H', 2, "blueprints", Nonce(1), "first"),
      header + TransitionFrame('H', 0, "blueprints", Nonce(1), "first"),
      header + TransitionFrame('F', 1, "blueprints"),
      held + TransitionFrame('H', 2, "blueprints", Nonce(2), "second"),
      released + TransitionFrame('F', 3, "blueprints")};
  for (const auto &bytes : invalid) {
    WriteBytes(directory.file(), bytes);
    REQUIRE(Throws<std::runtime_error>([&] {
      OwnershipLedger ledger(directory.file());
    }));
    REQUIRE(ReadBytes(directory.file()) == bytes);
    RequireUnlocked(directory.file());
  }
}

TEST_CASE("Ownership rejects relative paths and missing parent directories",
          "[admin][ownership]") {
  LedgerDirectory directory;
  REQUIRE(Throws<std::invalid_argument>([&] {
    OwnershipLedger ledger("relative-authority.ledger");
  }));
  const auto absent = std::filesystem::path(directory.file()).parent_path() /
                      "missing" / "authority.ledger";
  REQUIRE(Throws<std::runtime_error>([&] { OwnershipLedger ledger(absent.string()); }));
  REQUIRE_FALSE(std::filesystem::exists(absent.parent_path()));
}

TEST_CASE("Ownership partial append poisons the writer and refuses later recovery",
          "[admin][ownership]") {
  LedgerDirectory directory;
  std::string authority;
  {
    OwnershipLedger ledger(directory.file());
    authority = ledger.Inspect("blueprints").record.authority_uuid;
  }
  const auto original = ReadBytes(directory.file());
  const pid_t child = fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    try {
      OwnershipLedger ledger(directory.file());
      struct rlimit limit {};
      if (getrlimit(RLIMIT_FSIZE, &limit) != 0) {
        _exit(1);
      }
      const auto partial_limit = static_cast<rlim_t>(original.size() + 8);
      if (limit.rlim_max != RLIM_INFINITY && limit.rlim_max < partial_limit) {
        _exit(2);
      }
      limit.rlim_cur = partial_limit;
      if (signal(SIGXFSZ, SIG_IGN) == SIG_ERR ||
          setrlimit(RLIMIT_FSIZE, &limit) != 0) {
        _exit(3);
      }
      const bool append_failed = Throws<std::runtime_error>([&] {
        ledger.Acquire("blueprints", Nonce(1), "first", authority, "partial write");
      });
      const bool inspect_failed =
          Throws<std::runtime_error>([&] { ledger.Inspect("blueprints"); });
      _exit(append_failed && inspect_failed ? 0 : 4);
    } catch (...) {
      _exit(5);
    }
  }
  RequireChildSuccess(child);
  const auto damaged = ReadBytes(directory.file());
  REQUIRE(damaged.size() > original.size());
  REQUIRE(damaged.size() <= original.size() + 8);
  REQUIRE(damaged.substr(0, original.size()) == original);
  REQUIRE(Throws<std::runtime_error>([&] {
    OwnershipLedger ledger(directory.file());
  }));
  REQUIRE(ReadBytes(directory.file()) == damaged);
  RequireUnlocked(directory.file());
}

#endif

}  // namespace clio::run::admin

SIMPLE_TEST_MAIN();
