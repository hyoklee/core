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
/** @file ownership_tasks.h
 * @brief Wire tasks for the fixed-authority durable ownership service.
 */
#ifndef CLIO_ADMIN_OWNERSHIP_TASKS_H_
#define CLIO_ADMIN_OWNERSHIP_TASKS_H_

#include "clio_runtime/clio_runtime.h"
#include "autogen/admin_methods.h"

namespace clio::run::admin {

/** Ownership request and public response; the owner nonce is input-only. */
template <clio::run::u32 MethodId>
struct OwnershipTask : public clio::run::Task {
  IN clio::run::u64 authority_node_ = 0;
  IN clio::run::priv::string resource_;
  IN clio::run::priv::string expected_authority_uuid_;
  IN clio::run::u64 token_revision_ = 0;
  IN clio::run::priv::string owner_nonce_;
  IN clio::run::priv::string owner_label_input_;
  IN clio::run::priv::string holder_metadata_input_;
  OUT clio::run::priv::string outcome_;
  OUT clio::run::priv::string authority_uuid_;
  OUT clio::run::u64 revision_ = 0;
  OUT bool held_ = false;
  OUT clio::run::priv::string owner_label_;
  OUT clio::run::priv::string holder_metadata_;
  OUT clio::run::priv::string error_message_;

  /** Construct an empty task for wire deserialization. */
  OwnershipTask()
      : Task(), resource_(CLIO_PRIV_ALLOC),
        expected_authority_uuid_(CLIO_PRIV_ALLOC), owner_nonce_(CLIO_PRIV_ALLOC),
        owner_label_input_(CLIO_PRIV_ALLOC), holder_metadata_input_(CLIO_PRIV_ALLOC),
        outcome_(CLIO_PRIV_ALLOC), authority_uuid_(CLIO_PRIV_ALLOC),
        owner_label_(CLIO_PRIV_ALLOC), holder_metadata_(CLIO_PRIV_ALLOC),
        error_message_(CLIO_PRIV_ALLOC) {}

  /** Construct a request for its explicit physical authority node. */
  explicit OwnershipTask(const clio::run::TaskId& id,
                         const clio::run::PoolId& pool,
                         const clio::run::PoolQuery& query,
                         clio::run::u64 authority_node,
                         const std::string& resource,
                         const std::string& expected_authority_uuid,
                         clio::run::u64 token_revision,
                         const std::string& owner_nonce,
                         const std::string& owner_label,
                         const std::string& holder_metadata)
      : Task(id, pool, query, MethodId), authority_node_(authority_node),
        resource_(CLIO_PRIV_ALLOC, resource),
        expected_authority_uuid_(CLIO_PRIV_ALLOC, expected_authority_uuid),
        token_revision_(token_revision), owner_nonce_(CLIO_PRIV_ALLOC, owner_nonce),
        owner_label_input_(CLIO_PRIV_ALLOC, owner_label),
        holder_metadata_input_(CLIO_PRIV_ALLOC, holder_metadata),
        outcome_(CLIO_PRIV_ALLOC), authority_uuid_(CLIO_PRIV_ALLOC),
        owner_label_(CLIO_PRIV_ALLOC), holder_metadata_(CLIO_PRIV_ALLOC),
        error_message_(CLIO_PRIV_ALLOC) {
    task_id_ = id;
    pool_id_ = pool;
    method_ = MethodId;
    pool_query_ = query;
    task_flags_.Clear();
  }

  /** Serialize confidential request credentials only toward the authority. */
  template <typename Archive>
  CTP_CROSS_FUN void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(authority_node_, resource_, expected_authority_uuid_, token_revision_,
       owner_nonce_, owner_label_input_, holder_metadata_input_);
  }

  /** Serialize holder information without the owner nonce. */
  template <typename Archive>
  CTP_CROSS_FUN void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
    ar(outcome_, authority_uuid_, revision_, held_, owner_label_,
       holder_metadata_, error_message_);
  }

  /** Copy the complete task for runtime replication. */
  void Copy(const ctp::ipc::FullPtr<OwnershipTask>& other) {
    Task::Copy(other.template Cast<Task>());
    authority_node_ = other->authority_node_;
    resource_ = other->resource_;
    expected_authority_uuid_ = other->expected_authority_uuid_;
    token_revision_ = other->token_revision_;
    owner_nonce_ = other->owner_nonce_;
    owner_label_input_ = other->owner_label_input_;
    holder_metadata_input_ = other->holder_metadata_input_;
    CopyResult(other);
  }

  /** Aggregate authority outputs while preserving the origin's task IDs. */
  void AggregateOut(const ctp::ipc::FullPtr<clio::run::Task>& base) {
    Task::AggregateOut(base);
    CopyResult(base.template Cast<OwnershipTask>());
  }

 private:
  void CopyResult(const ctp::ipc::FullPtr<OwnershipTask>& other) {
    outcome_ = other->outcome_;
    authority_uuid_ = other->authority_uuid_;
    revision_ = other->revision_;
    held_ = other->held_;
    owner_label_ = other->owner_label_;
    holder_metadata_ = other->holder_metadata_;
    error_message_ = other->error_message_;
  }
};

using OwnershipAcquireTask = OwnershipTask<Method::kOwnershipAcquire>;
using OwnershipReleaseTask = OwnershipTask<Method::kOwnershipRelease>;
using OwnershipInspectTask = OwnershipTask<Method::kOwnershipInspect>;

}  // namespace clio::run::admin

#endif  // CLIO_ADMIN_OWNERSHIP_TASKS_H_
