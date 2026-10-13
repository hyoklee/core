/*
 * Copyright (c) 2026, IOWarp contributors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
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
/** @file ownership_python.h
 * @brief Existing-wheel binding for native fixed-authority ownership.
 */
#ifndef CLIO_OWNERSHIP_PYTHON_H_
#define CLIO_OWNERSHIP_PYTHON_H_

#include <clio_runtime/admin/admin_client.h>
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <stdexcept>

namespace clio::run::admin {

/** Require explicit client initialization without bootstrapping a runtime. */
inline void RequireOwnershipClient() {
  auto* ipc = CLIO_CPU_IPC;
  if (ipc == nullptr || !ipc->IsInitialized()) {
    throw std::runtime_error("ownership requires explicit core client initialization");
  }
}

/** Wait for a real authority response and return only redacted public fields. */
template <typename T>
nanobind::dict AwaitOwnership(clio::run::Future<T>& future) {
  if (future.IsNull()) {
    throw std::runtime_error("ownership authority request was not submitted");
  }
  bool completed;
  {
    nanobind::gil_scoped_release release;
    completed = future.Wait(10.0f);
  }
  if (!completed) {
    throw std::runtime_error(
        "ownership RPC did not complete; retain the nonce and retry the same request");
  }
  if (future->GetReturnCode() != 0 || future->outcome_.empty() ||
      future->outcome_.str() == "error") {
    throw std::runtime_error("ownership authority rejected the operation: " +
                             future->error_message_.str());
  }
  nanobind::dict result;
  result["outcome"] = future->outcome_.str();
  result["authority_uuid"] = future->authority_uuid_.str();
  result["revision"] = future->revision_;
  result["held"] = future->held_;
  result["owner_label"] = future->owner_label_.str();
  result["holder_metadata"] = future->holder_metadata_.str();
  return result;
}

/** Register coordination in the shipped CTE extension, without CTE storage. */
inline void RegisterOwnershipBindings(nanobind::module_& module) {
  namespace nb = nanobind;
  using namespace nanobind::literals;
  module.def("ownership_capabilities", []() {
    nb::dict result;
    result["protocol"] = 1;
    result["non_expiring"] = true;
    result["physical_authority"] = true;
    result["authority_platform"] = "linux";
    return result;
  }, "Describe this binding's fixed-authority, non-expiring ownership protocol.");
  module.def("ownership_inspect", [](uint64_t node, const std::string& resource) {
    RequireOwnershipClient();
    Client client;
    auto future = client.AsyncOwnershipInspect(node, resource);
    return AwaitOwnership(future);
  }, "authority_node_id"_a, "resource_key"_a,
     "Inspect one physical authority without revealing its owner nonce.");
  module.def("ownership_acquire", [](
      uint64_t node, const std::string& resource, const std::string& expected_uuid,
      const std::string& nonce, const std::string& label, const std::string& metadata) {
    RequireOwnershipClient();
    Client client;
    auto future = client.AsyncOwnershipAcquire(
        node, resource, expected_uuid, nonce, label, metadata);
    return AwaitOwnership(future);
  }, "authority_node_id"_a, "resource_key"_a, "expected_authority_uuid"_a,
     "owner_nonce"_a, "owner_label"_a, "holder_metadata"_a,
     "Acquire durable ownership without expiry. Retry an uncertain RPC with the same nonce.");
  module.def("ownership_release", [](
      uint64_t node, const std::string& resource, const std::string& expected_uuid,
      uint64_t revision, const std::string& nonce) {
    RequireOwnershipClient();
    Client client;
    auto future = client.AsyncOwnershipRelease(
        node, resource, expected_uuid, revision, nonce);
    return AwaitOwnership(future);
  }, "authority_node_id"_a, "resource_key"_a, "expected_authority_uuid"_a,
     "revision"_a, "owner_nonce"_a,
     "Release only an exact ownership token, after a durable FREE transition.");
}

}  // namespace clio::run::admin

#endif  // CLIO_OWNERSHIP_PYTHON_H_
