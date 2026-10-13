"""Test ownership protocol validation without creating a runtime or an authority.

CI runs these tests against the freshly installed wheel. Native transport mocks
exist only in this test module; the native ledger has separate real-I/O tests.
"""

from __future__ import annotations

import importlib
import json
import unittest
from dataclasses import replace
from types import SimpleNamespace
from typing import Any
from unittest import mock

from iowarp_core import ownership

AUTHORITY = "986f1511-413e-4efe-a8b4-7fc82c665fa2"
OTHER_AUTHORITY = "fc85233b-2f4c-4ab2-9276-4d7da4763ef4"
RESOURCE = "blueprints:documented-store-uuid:mutations"
NONCE = "a" * 64


def authority_reply(
    outcome: str,
    *,
    held: bool = True,
    revision: int = 1,
    authority_uuid: str = AUTHORITY,
) -> dict[str, Any]:
    """Construct a native response fixture with no ownership nonce."""
    return {
        "outcome": outcome,
        "authority_uuid": authority_uuid,
        "revision": revision,
        "held": held,
        "owner_label": "agent at worker-a" if held else "",
        "holder_metadata": json.dumps({"host": "worker-a", "pid": 123}) if held else "{}",
    }


class OwnershipNativeSurfaceTests(unittest.TestCase):
    """Verify that each freshly built wheel exports the real native protocol."""

    def test_native_protocol_is_exported_without_initializing_a_runtime(self) -> None:
        native = importlib.import_module("iowarp_core").clio_cte_core_ext
        capabilities = native.ownership_capabilities()
        self.assertEqual(type(capabilities["protocol"]), int)
        self.assertEqual(capabilities["protocol"], 1)
        self.assertIs(capabilities["non_expiring"], True)
        for name in ("ownership_inspect", "ownership_acquire", "ownership_release"):
            with self.subTest(function=name):
                self.assertTrue(callable(getattr(native, name)))


class OwnershipClientTests(unittest.TestCase):
    """Exercise the public client, including fail-closed protocol boundaries."""

    def setUp(self) -> None:
        """Replace only the native transport in the installed production wrapper."""
        self.native = SimpleNamespace(
            ownership_capabilities=mock.Mock(return_value={"protocol": 1, "non_expiring": True}),
            ownership_inspect=mock.Mock(return_value=authority_reply("inspected")),
            ownership_acquire=mock.Mock(return_value=authority_reply("acquired")),
            ownership_release=mock.Mock(
                return_value=authority_reply("released", held=False, revision=2)
            ),
        )
        package = SimpleNamespace(clio_cte_core_ext=self.native)
        patcher = mock.patch.object(ownership.importlib, "import_module", return_value=package)
        self.addCleanup(patcher.stop)
        patcher.start()
        self.client = ownership.OwnershipClient(0, AUTHORITY)

    def acquire(self) -> ownership.OwnershipToken:
        """Acquire the standard fixture and return its exact release credential."""
        result = self.client.try_acquire(RESOURCE, NONCE, "agent", {"host": "worker-a"})
        self.assertTrue(result.acquired)
        self.assertIsNotNone(result.token)
        assert result.token is not None
        return result.token

    def test_configuration_accepts_zero_based_u32_nodes_without_native_calls(self) -> None:
        for value in (0, 1, 2**32 - 1):
            with self.subTest(node=value):
                client = ownership.OwnershipClient(value, AUTHORITY)
                self.assertEqual(client.authority_node_id, value)
        for value in (-1, 2**32, True, "0"):
            with self.subTest(invalid_node=value), self.assertRaises(ValueError):
                ownership.OwnershipClient(value, AUTHORITY)
        self.native.ownership_capabilities.assert_not_called()

    def test_configuration_requires_explicit_canonical_authority_uuid(self) -> None:
        for value in ("", "invalid", AUTHORITY.upper(), None):
            with self.subTest(uuid=value), self.assertRaises(ValueError):
                ownership.OwnershipClient(0, value)
        self.native.ownership_inspect.assert_not_called()

    def test_missing_or_unsupported_capability_never_calls_acquire(self) -> None:
        for capabilities in (
            {},
            {"protocol": 2, "non_expiring": True},
            {"protocol": True, "non_expiring": True},
            {"protocol": 1, "non_expiring": False},
            {"protocol": 1, "non_expiring": 1},
        ):
            with self.subTest(capabilities=capabilities):
                self.native.ownership_capabilities.return_value = capabilities
                with self.assertRaises(ownership.OwnershipUnavailable):
                    self.client.try_acquire(RESOURCE, NONCE, "agent", {})
        self.native.ownership_acquire.assert_not_called()

    def test_missing_native_function_fails_closed(self) -> None:
        del self.native.ownership_acquire
        with self.assertRaises(ownership.OwnershipUnavailable):
            self.client.try_acquire(RESOURCE, NONCE, "agent", {})

    def test_acquisition_returns_exact_token_and_hides_nonce_in_representations(self) -> None:
        token = self.acquire()
        self.assertEqual(token.resource_key, RESOURCE)
        self.assertEqual(token.authority_node_id, 0)
        self.assertEqual(token.authority_uuid, AUTHORITY)
        self.assertEqual(token.revision, 1)
        self.assertEqual(token.owner_nonce, NONCE)
        self.assertNotIn(NONCE, repr(token))
        args = self.native.ownership_acquire.call_args.args
        self.assertEqual(args[:5], (0, RESOURCE, AUTHORITY, NONCE, "agent"))
        self.assertEqual(json.loads(args[5]), {"host": "worker-a"})

    def test_busy_returns_inspectable_holder_without_token(self) -> None:
        self.native.ownership_acquire.return_value = authority_reply("busy", revision=7)
        result = self.client.try_acquire(RESOURCE, NONCE, "agent", {})
        self.assertFalse(result.acquired)
        self.assertIsNone(result.token)
        self.assertEqual(result.holder.revision, 7)
        self.assertEqual(result.holder.holder_metadata["host"], "worker-a")
        self.assertFalse(hasattr(result.holder, "owner_nonce"))

    def test_input_limits_fail_before_any_native_acquisition(self) -> None:
        invalid_inputs = (
            ("", NONCE, "agent", {}),
            ("bad\0resource", NONCE, "agent", {}),
            ("é" * 513, NONCE, "agent", {}),
            (RESOURCE, "b" * 63, "agent", {}),
            (RESOURCE, "A" * 64, "agent", {}),
            (RESOURCE, NONCE, "", {}),
            (RESOURCE, NONCE, "é" * 129, {}),
            (RESOURCE, NONCE, "agent", []),
            (RESOURCE, NONCE, "agent", {"oversized": "x" * 8192}),
            (RESOURCE, NONCE, "agent", {"invalid_number": float("nan")}),
        )
        for values in invalid_inputs:
            with self.subTest(values=values[:3]), self.assertRaises(ValueError):
                self.client.try_acquire(*values)
        self.native.ownership_acquire.assert_not_called()

    def test_malformed_native_response_never_establishes_ownership(self) -> None:
        malformed = (
            None,
            {},
            authority_reply("acquired", held=False),
            authority_reply("unexpected"),
            authority_reply("acquired", revision=0),
            authority_reply("acquired", revision=True),
            authority_reply("acquired", revision=2**64),
            {**authority_reply("acquired"), "holder_metadata": "[]"},
            {**authority_reply("acquired"), "holder_metadata": "not-json"},
            {**authority_reply("acquired"), "owner_label": ""},
        )
        for reply in malformed:
            with self.subTest(reply=reply):
                self.native.ownership_acquire.return_value = reply
                with self.assertRaises(ownership.OwnershipUnavailable):
                    self.client.try_acquire(RESOURCE, NONCE, "agent", {})

    def test_inspection_never_rebinds_a_changed_authority(self) -> None:
        self.native.ownership_inspect.return_value = authority_reply(
            "inspected", authority_uuid=OTHER_AUTHORITY
        )
        setup = ownership.OwnershipClient.inspect_authority(0)
        self.assertEqual(setup.authority_uuid, OTHER_AUTHORITY)
        with self.assertRaises(ownership.OwnershipUnavailable):
            self.client.inspect(RESOURCE)
        self.assertEqual(self.client.expected_authority_uuid, AUTHORITY)

    def test_acquisition_rejects_a_different_authority(self) -> None:
        self.native.ownership_acquire.return_value = authority_reply(
            "acquired", authority_uuid=OTHER_AUTHORITY
        )
        with self.assertRaises(ownership.OwnershipUnavailable):
            self.client.try_acquire(RESOURCE, NONCE, "agent", {})

    def test_uncertain_acquisition_can_retry_the_same_nonce(self) -> None:
        self.native.ownership_acquire.side_effect = [
            RuntimeError("ownership RPC timed out; outcome is uncertain"),
            authority_reply("acquired", revision=3),
        ]
        with self.assertRaises(ownership.OwnershipUnavailable):
            self.client.try_acquire(RESOURCE, NONCE, "agent", {})
        result = self.client.try_acquire(RESOURCE, NONCE, "agent", {})
        self.assertEqual(result.token.owner_nonce, NONCE)
        self.assertEqual(result.token.revision, 3)
        for call in self.native.ownership_acquire.call_args_list:
            self.assertEqual(call.args[3], NONCE)

    def test_release_sends_only_the_exact_token(self) -> None:
        token = self.acquire()
        self.client.release(token)
        self.native.ownership_release.assert_called_once_with(0, RESOURCE, AUTHORITY, 1, NONCE)

    def test_foreign_or_invalid_tokens_never_reach_native_release(self) -> None:
        token = self.acquire()
        for changed in (
            replace(token, authority_node_id=1),
            replace(token, authority_uuid=OTHER_AUTHORITY),
        ):
            with self.subTest(token=changed), self.assertRaises(ownership.OwnershipConflict):
                self.client.release(changed)
        for revision in (0, -1, True, 2**64):
            with self.subTest(revision=revision), self.assertRaises(ValueError):
                self.client.release(replace(token, revision=revision))
        self.native.ownership_release.assert_not_called()

    def test_rejected_release_is_a_conflict_and_cannot_clear_another_holder(self) -> None:
        token = self.acquire()
        self.native.ownership_release.return_value = authority_reply("rejected", revision=9)
        with self.assertRaises(ownership.OwnershipConflict):
            self.client.release(token)
        self.assertEqual(token.revision, 1)
        self.assertEqual(token.owner_nonce, NONCE)

    def test_release_requires_a_confirmed_durable_free_revision(self) -> None:
        token = self.acquire()
        for reply in (
            authority_reply("released", revision=2),
            authority_reply("released", held=False, revision=1),
            authority_reply("unknown", held=False, revision=2),
            authority_reply("released", held=False, revision=2, authority_uuid=OTHER_AUTHORITY),
        ):
            with self.subTest(reply=reply):
                self.native.ownership_release.return_value = reply
                with self.assertRaises(ownership.OwnershipUnavailable):
                    self.client.release(token)

    def test_unknown_release_outcome_retains_the_callers_exact_token(self) -> None:
        token = self.acquire()
        self.native.ownership_release.side_effect = [
            RuntimeError("ownership RPC timed out; outcome is uncertain"),
            authority_reply("released", held=False, revision=2),
        ]
        with self.assertRaises(ownership.OwnershipUnavailable):
            self.client.release(token)
        self.client.release(token)
        for call in self.native.ownership_release.call_args_list:
            self.assertEqual(call.args, (0, RESOURCE, AUTHORITY, 1, NONCE))


if __name__ == "__main__":
    unittest.main()
