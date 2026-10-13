/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
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

/** @file Test ownership through real Admin RPC and independent client exits. */
#include <clio_runtime/admin/admin_client.h>
#include <clio_runtime/clio_runtime.h>
#include <clio_runtime/ipc_manager.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "runtime_server.h"
#include "simple_test.h"

#if !defined(__linux__)
#error "The native ownership authority RPC test requires Linux"
#endif

extern char** environ;

namespace {

constexpr int kOwnershipRpcPort = 19841;
constexpr const char* kResource = "blueprints:rpc-test-store:mutations";

/** Own a fresh, private test directory on the configured persistent CI disk. */
class PrivateDirectory {
 public:
  PrivateDirectory() {
    const char* configured = std::getenv("CLIO_TEST_DATA_DIR");
    auto parent = configured ? std::filesystem::path(configured)
                             : std::filesystem::current_path();
    parent = std::filesystem::absolute(parent);
    std::filesystem::create_directories(parent);
    auto pattern = (parent / "ownership-rpc-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* created = ::mkdtemp(buffer.data());
    if (created == nullptr) {
      throw std::runtime_error("cannot create private ownership RPC directory");
    }
    path_ = created;
  }

  ~PrivateDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    if (error) {
      std::cerr << "ownership RPC directory cleanup failed: " << error.message()
                << '\n';
    }
  }

  /** Return the absolute private directory path. */
  std::string path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

/** Restore one environment variable after the isolated runtime stops. */
class EnvironmentValue {
 public:
  EnvironmentValue(std::string name, const std::string& value)
      : name_(std::move(name)) {
    const char* previous = std::getenv(name_.c_str());
    if (previous != nullptr) {
      previous_ = previous;
    }
    if (::setenv(name_.c_str(), value.c_str(), 1) != 0) {
      throw std::runtime_error("cannot configure ownership RPC environment");
    }
  }

  ~EnvironmentValue() {
    const int result = previous_ ? ::setenv(name_.c_str(), previous_->c_str(), 1)
                                 : ::unsetenv(name_.c_str());
    if (result != 0) {
      std::cerr << "ownership RPC environment restore failed\n";
    }
  }

 private:
  std::string name_;
  std::optional<std::string> previous_;
};

/** Wait for this namespaced TCP daemon's real Admin-pool readiness marker. */
void RequireAuthorityReady(clio::run::test::RuntimeServer& server,
                           const std::string& log_path) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!server.IsRunning()) {
      FAIL("ownership authority exited during startup\n"
           << clio::run::test::RuntimeServer::LogTail(80));
    }
    if (clio::run::test::RuntimeServer::ServerLogHasAdminPool(log_path)) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  FAIL("ownership authority readiness timed out\n"
       << clio::run::test::RuntimeServer::LogTail(80));
}

/** Require a completed real RPC before reading its response fields. */
template <typename Future>
void RequireCompleted(Future& future) {
  REQUIRE_FALSE(future.IsNull());
  REQUIRE(future.Wait(10.0f));
  REQUIRE(future->GetReturnCode() == 0);
}

/** Run a separate client against the already-running authority. */
int IndependentClient(const std::string& mode, const std::string& resource,
                      const std::string& authority,
                      const std::string& nonce) {
  try {
    if (!clio::run::CLIO_INIT(clio::run::RuntimeMode::kClient, false)) {
      return 2;
    }
    clio::run::admin::Client client;
    auto acquired = client.AsyncOwnershipAcquire(
        0, resource, authority, nonce, "independent client", "{\"role\":\"child\"}");
    RequireCompleted(acquired);
    const auto expected = mode == "busy" ? "busy" : "acquired";
    if (acquired->outcome_.str() != expected) {
      return 3;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "independent ownership RPC client failed: " << error.what()
              << '\n';
    return 4;
  }
}

/** Spawn with exec rather than fork a process containing active ZMQ threads. */
void RunIndependentClient(const std::string& mode, const std::string& authority,
                          const std::string& nonce) {
  std::string executable = CLIO_OWNERSHIP_TEST_EXE;
  std::string child_flag = "--ownership-child";
  std::string resource = kResource;
  std::vector<std::string> arguments = {
      executable, child_flag, mode, resource, authority, nonce};
  std::vector<char*> argv;
  for (auto& argument : arguments) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);
  pid_t child = -1;
  const int spawned = ::posix_spawn(&child, executable.c_str(), nullptr, nullptr,
                                    argv.data(), environ);
  if (spawned != 0) {
    throw std::runtime_error(std::string("cannot spawn ownership client: ") +
                             std::strerror(spawned));
  }
  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  REQUIRE(waited == child);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
}

}  // namespace

TEST_CASE("Ownership RPC preserves claims after an independent client exits",
          "[admin][ownership][integration]") {
  PrivateDirectory directory;
  std::filesystem::create_directories(directory.path() + "/runtime");
  EnvironmentValue authority_node("CLIO_OWNERSHIP_AUTHORITY_NODE", "0");
  EnvironmentValue ownership_root("CLIO_OWNERSHIP_STORAGE_ROOT", directory.path());
  EnvironmentValue storage_root("CLIO_STORAGE_ROOT", directory.path() + "/runtime");
  EnvironmentValue namespace_value("CLIO_IPC_NAMESPACE",
                                   "ownership-rpc-" + std::to_string(::getpid()));
  EnvironmentValue client_mode("CLIO_WITH_RUNTIME", "0");
  EnvironmentValue transport("CLIO_IPC_MODE", "TCP");
  const std::string log_path = directory.path() + "/authority.log";
  EnvironmentValue server_log("CLIO_TEST_SERVER_LOG", log_path);
  EnvironmentValue log_level("CTP_LOG_LEVEL", "info");
  clio::run::test::RuntimeServer server;
  REQUIRE(server.Start(kOwnershipRpcPort));
  // RuntimeServer::WaitForReady probes an unnamespaced shared-memory name.
  // This client uses TCP and must keep its explicitly isolated namespace.
  RequireAuthorityReady(server, log_path);
  REQUIRE(clio::run::CLIO_INIT(clio::run::RuntimeMode::kClient, false));
  clio::run::admin::Client client;

  auto inspected = client.AsyncOwnershipInspect(0, kResource);
  RequireCompleted(inspected);
  REQUIRE(inspected->outcome_.str() == "inspected");
  const std::string authority = inspected->authority_uuid_.str();
  REQUIRE_FALSE(authority.empty());
  const std::string first_nonce(64, 'a');
  auto first = client.AsyncOwnershipAcquire(0, kResource, authority, first_nonce,
                                           "parent client", "{\"role\":\"parent\"}");
  RequireCompleted(first);
  REQUIRE(first->outcome_.str() == "acquired");
  REQUIRE(first->held_);
  const auto first_revision = first->revision_;

  RunIndependentClient("busy", authority, std::string(64, 'b'));
  auto released = client.AsyncOwnershipRelease(0, kResource, authority,
                                               first_revision, first_nonce);
  RequireCompleted(released);
  REQUIRE(released->outcome_.str() == "released");
  REQUIRE_FALSE(released->held_);

  const std::string child_nonce(64, 'c');
  RunIndependentClient("claim", authority, child_nonce);
  auto after_exit = client.AsyncOwnershipInspect(0, kResource);
  RequireCompleted(after_exit);
  REQUIRE(after_exit->held_);
  REQUIRE(after_exit->owner_label_.str() == "independent client");
  REQUIRE(after_exit->revision_ > first_revision);
  REQUIRE(after_exit->authority_uuid_.str() == authority);

  auto stale = client.AsyncOwnershipRelease(0, kResource, authority,
                                            first_revision, first_nonce);
  RequireCompleted(stale);
  REQUIRE(stale->outcome_.str() == "rejected");
  REQUIRE(stale->held_);
  REQUIRE(stale->revision_ == after_exit->revision_);
  auto blocked = client.AsyncOwnershipAcquire(0, kResource, authority, first_nonce,
                                             "parent client", "{}");
  RequireCompleted(blocked);
  REQUIRE(blocked->outcome_.str() == "busy");

  // waitpid above proves this test's former child writer stopped. Recovery
  // still requires its exact retained nonce and revision; there is no steal.
  auto recovered = client.AsyncOwnershipRelease(0, kResource, authority,
                                                after_exit->revision_, child_nonce);
  RequireCompleted(recovered);
  REQUIRE(recovered->outcome_.str() == "released");
  REQUIRE_FALSE(recovered->held_);
  REQUIRE(recovered->revision_ > after_exit->revision_);
}

/** Run the real-client child mode before the normal isolated test harness. */
int main(int argc, char* argv[]) {
  if (argc == 6 && std::string(argv[1]) == "--ownership-child") {
    const int result = IndependentClient(argv[2], argv[3], argv[4], argv[5]);
    ::_exit(result);
  }
  SIMPLE_TEST_SETENV("CLIO_UNIT_TEST", "1");
  const std::string filter = argc > 1 ? argv[1] : "";
  const int result = SimpleTest::run_all_tests(filter);
  SIMPLE_TEST_PROCESS_EXIT(result);
  if (SimpleTest::g_test_finalize) {
    SimpleTest::g_test_finalize();
  }
  return result;
}
