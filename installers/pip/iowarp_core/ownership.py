# Copyright (c) 2026, IOWarp contributors. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
# 3. Neither the name of the copyright holder nor the names of its contributors
#    may be used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
"""Non-expiring ownership at an explicitly selected shared core authority.

The core client must already be initialized against the configured hostfile.
This module never starts a runtime, discovers a replacement authority, expires
ownership, or releases another owner's token. Authority inspection is a setup
operation: persist its UUID in configuration before constructing a client.
"""

from __future__ import annotations

import importlib
import json
import re
from dataclasses import dataclass, field
from typing import Any
from uuid import UUID


class OwnershipError(RuntimeError):
    """Base error for an ownership operation that did not establish success."""


class OwnershipUnavailable(OwnershipError):
    """The configured authority or native coordination capability is unavailable."""


class OwnershipConflict(OwnershipError):
    """A release did not match the current authority/resource/revision/nonce."""


@dataclass(frozen=True, slots=True)
class OwnershipHolder:
    """Public, inspectable authority and holder information without a nonce."""

    authority_uuid: str
    revision: int
    held: bool
    owner_label: str
    holder_metadata: dict[str, Any]


@dataclass(frozen=True, slots=True)
class OwnershipToken:
    """Exact acquisition credential; retain it for explicit release or retry."""

    resource_key: str
    authority_node_id: int
    authority_uuid: str
    revision: int
    owner_nonce: str = field(repr=False)


@dataclass(frozen=True, slots=True)
class OwnershipAcquireResult:
    """Acquisition outcome with a token only when this caller owns the resource."""

    acquired: bool
    token: OwnershipToken | None
    holder: OwnershipHolder


def _node(value: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < 2**32:
        raise ValueError("authority_node_id must be an unsigned 32-bit node ID")
    return value


def _uuid(value: str) -> str:
    if not isinstance(value, str) or str(UUID(value)) != value:
        raise ValueError("expected_authority_uuid must be a canonical lowercase UUID")
    return value


def _resource(value: str) -> str:
    if not isinstance(value, str) or not value or "\0" in value:
        raise ValueError("resource_key must be a nonempty stable identity")
    if len(value.encode("utf-8")) > 1024:
        raise ValueError("resource_key exceeds 1024 UTF-8 bytes")
    return value


def _nonce(value: str) -> str:
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None:
        raise ValueError("owner_nonce must be secrets.token_hex(32), retained for retries")
    return value


def _native() -> Any:
    try:
        native = importlib.import_module("iowarp_core").clio_cte_core_ext
        capabilities = native.ownership_capabilities()
    except (ImportError, AttributeError, RuntimeError) as exc:
        raise OwnershipUnavailable("native core ownership capability is unavailable") from exc
    if (
        not isinstance(capabilities, dict)
        or type(capabilities.get("protocol")) is not int
        or capabilities["protocol"] != 1
    ):
        raise OwnershipUnavailable("unsupported native core ownership protocol")
    if capabilities.get("non_expiring") is not True:
        raise OwnershipUnavailable("native core does not guarantee non-expiring ownership")
    return native


def _holder(reply: dict[str, Any]) -> OwnershipHolder:
    try:
        authority = _uuid(reply["authority_uuid"])
        revision = reply["revision"]
        held = reply["held"]
        label = reply["owner_label"]
        metadata = json.loads(reply["holder_metadata"] or "{}")
    except (KeyError, TypeError, ValueError) as exc:
        raise OwnershipUnavailable("invalid ownership authority response") from exc
    if (
        isinstance(revision, bool)
        or not isinstance(revision, int)
        or not 0 <= revision < 2**64
        or not isinstance(held, bool)
        or not isinstance(label, str)
        or not isinstance(metadata, dict)
        or (held and (revision == 0 or not label))
        or (not held and (label or metadata))
    ):
        raise OwnershipUnavailable("inconsistent ownership authority response")
    return OwnershipHolder(authority, revision, held, label, metadata)


def _call(method: str, *args: Any) -> dict[str, Any]:
    try:
        reply = getattr(_native(), method)(*args)
    except (RuntimeError, AttributeError) as exc:
        raise OwnershipUnavailable("configured ownership authority operation failed") from exc
    if not isinstance(reply, dict):
        raise OwnershipUnavailable("ownership authority returned an invalid response")
    return reply


class OwnershipClient:
    """Use one fixed physical core authority whose UUID is explicitly bound."""

    def __init__(self, authority_node_id: int, expected_authority_uuid: str) -> None:
        """Validate configuration without adopting an inspected or new UUID."""
        self.authority_node_id = _node(authority_node_id)
        self.expected_authority_uuid = _uuid(expected_authority_uuid)

    @staticmethod
    def inspect_authority(
        authority_node_id: int, resource_key: str = "__authority__"
    ) -> OwnershipHolder:
        """Inspect for operator setup; never silently bind or replace an authority."""
        reply = _call("ownership_inspect", _node(authority_node_id), _resource(resource_key))
        if reply.get("outcome") != "inspected":
            raise OwnershipUnavailable("authority inspection did not complete")
        return _holder(reply)

    def inspect(self, resource_key: str) -> OwnershipHolder:
        """Inspect a holder, rejecting a different or reinitialized authority."""
        holder = self.inspect_authority(self.authority_node_id, resource_key)
        self._check_authority(holder)
        return holder

    def try_acquire(
        self,
        resource_key: str,
        owner_nonce: str,
        owner_label: str,
        holder_metadata: dict[str, Any],
    ) -> OwnershipAcquireResult:
        """Acquire once, or retry the same nonce after an uncertain RPC result."""
        resource_key = _resource(resource_key)
        owner_nonce = _nonce(owner_nonce)
        if not isinstance(owner_label, str) or not owner_label:
            raise ValueError("owner_label must identify the holder")
        if len(owner_label.encode("utf-8")) > 256:
            raise ValueError("owner_label exceeds 256 UTF-8 bytes")
        if not isinstance(holder_metadata, dict):
            raise ValueError("holder_metadata must be a JSON object without secrets")
        metadata = json.dumps(holder_metadata, ensure_ascii=False, allow_nan=False)
        if len(metadata.encode("utf-8")) > 8192:
            raise ValueError("holder_metadata exceeds 8192 UTF-8 bytes")
        reply = _call(
            "ownership_acquire",
            self.authority_node_id,
            resource_key,
            self.expected_authority_uuid,
            owner_nonce,
            owner_label,
            metadata,
        )
        holder = _holder(reply)
        self._check_authority(holder)
        if reply.get("outcome") not in {"acquired", "busy"} or not holder.held:
            raise OwnershipUnavailable("authority did not return a valid acquisition outcome")
        acquired = reply["outcome"] == "acquired"
        token = None
        if acquired:
            token = OwnershipToken(
                resource_key,
                self.authority_node_id,
                holder.authority_uuid,
                holder.revision,
                owner_nonce,
            )
        return OwnershipAcquireResult(acquired, token, holder)

    def release(self, token: OwnershipToken) -> None:
        """Release an exact token; reject stale ownership and every force-release attempt."""
        if not isinstance(token, OwnershipToken):
            raise TypeError("release requires an OwnershipToken")
        if (
            token.authority_node_id != self.authority_node_id
            or token.authority_uuid != self.expected_authority_uuid
        ):
            raise OwnershipConflict("token belongs to a different configured authority")
        if (
            isinstance(token.revision, bool)
            or not isinstance(token.revision, int)
            or not 0 < token.revision < 2**64
        ):
            raise ValueError("token revision must be a positive unsigned 64-bit integer")
        reply = _call(
            "ownership_release",
            self.authority_node_id,
            _resource(token.resource_key),
            self.expected_authority_uuid,
            token.revision,
            _nonce(token.owner_nonce),
        )
        holder = _holder(reply)
        self._check_authority(holder)
        if reply.get("outcome") == "rejected":
            raise OwnershipConflict("release does not match the current ownership token")
        if reply.get("outcome") != "released" or holder.held or holder.revision <= token.revision:
            raise OwnershipUnavailable("authority did not confirm a durable release")

    def _check_authority(self, holder: OwnershipHolder) -> None:
        if holder.authority_uuid != self.expected_authority_uuid:
            raise OwnershipUnavailable("configured ownership authority UUID changed")
