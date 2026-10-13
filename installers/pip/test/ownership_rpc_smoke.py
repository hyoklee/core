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
"""Exercise real ownership RPC through an installed Linux wheel in CI only."""

from __future__ import annotations

import contextlib
import json
import os
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def _require(condition: bool, message: str) -> None:
    """Fail explicitly even if Python assertion optimization is enabled."""
    if not condition:
        raise AssertionError(message)


def _unused_port_cluster() -> int:
    """Find a local base port whose complete runtime port cluster is free."""
    for _ in range(32):
        port = 20000 + secrets.randbelow(10000)
        try:
            with contextlib.ExitStack() as stack:
                for offset in range(4):
                    connection = stack.enter_context(socket.socket())
                    connection.bind(("127.0.0.1", port + offset))
            return port
        except OSError:
            continue
    raise RuntimeError("No isolated local runtime port cluster is available")


def _configure(root: Path, port: int) -> None:
    """Require a private authority and one explicit shared client/server config."""
    runtime_root = root / "runtime"
    runtime_root.mkdir(mode=0o700)
    hostfile = root / "hosts"
    hostfile.write_text(f"127.0.0.1:{port}\n", encoding="utf-8")
    namespace = f"ownership-wheel-{secrets.token_hex(12)}"
    config = root / "runtime.yaml"
    config.write_text(
        f"networking:\n  port: {port}\n  hostfile: {json.dumps(str(hostfile))}\n"
        f"runtime:\n  num_threads: 2\n  conf_dir: {json.dumps(str(runtime_root))}\n"
        f"  ipc_namespace: {json.dumps(namespace)}\n"
        "compose: []\nviz:\n  enabled: false\n",
        encoding="utf-8",
    )
    os.environ.update(
        CLIO_OWNERSHIP_AUTHORITY_NODE="0",
        CLIO_OWNERSHIP_STORAGE_ROOT=str(root),
        CLIO_STORAGE_ROOT=str(runtime_root),
        CLIO_SERVER_CONF=str(config),
        CLIO_PORT=str(port),
        CLIO_BIND_ADDR="127.0.0.1",
        CLIO_SERVER_ADDR="127.0.0.1",
        CLIO_IPC_NAMESPACE=namespace,
        CLIO_IPC_MODE="TCP",
        CLIO_WITH_RUNTIME="0",
        CLIO_WAIT_SERVER="15",
        CLIO_VIZ_ENABLE="0",
        CTP_LOG_LEVEL="info",
    )


def _wait_ready(daemon: subprocess.Popen[bytes], log: Path) -> None:
    """Require the owned daemon to create its Admin pool before the deadline."""
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        _require(daemon.poll() is None, "Owned runtime exited before becoming ready")
        if "Admin chimod pool created successfully" in log.read_text(
            encoding="utf-8", errors="replace"
        ):
            return
        time.sleep(0.1)
    raise TimeoutError("Owned runtime did not create its Admin pool in 30 seconds")


def _require_wheel_libraries(pid: int, library_dir: Path, names: tuple[str, ...]) -> None:
    """Verify the real process mapped core libraries from this installed wheel."""
    mappings = Path(f"/proc/{pid}/maps").read_text(encoding="utf-8").splitlines()
    for name in names:
        paths = [
            Path(line.split(maxsplit=5)[5])
            for line in mappings
            if len(line.split(maxsplit=5)) == 6
            and Path(line.split(maxsplit=5)[5]).name.startswith(name)
        ]
        _require(bool(paths), f"Owned process did not load {name}")
        _require(
            all(path.resolve().parent == library_dir.resolve() for path in paths),
            f"Owned process loaded {name} outside the installed wheel: {paths}",
        )


def _client_smoke() -> None:
    """Initialize one real client and exercise successful and rejected native replies."""
    from iowarp_core import clio_cte_core_ext as native
    from iowarp_core import get_lib_dir
    from iowarp_core.ownership import OwnershipClient, OwnershipConflict

    _require(native.clio_init(native.RuntimeMode.kClient, False), "Client attach failed")
    _require_wheel_libraries(
        os.getpid(), Path(get_lib_dir()), ("libclio_run_cxx.so", "libclio_admin_client.so")
    )
    resource = "blueprint-store:installed-wheel-native-rpc"
    identity = OwnershipClient.inspect_authority(0)
    client = OwnershipClient(0, identity.authority_uuid)
    first_nonce = secrets.token_hex(32)
    first = client.try_acquire(resource, first_nonce, "wheel client", {"pid": os.getpid()})
    _require(first.acquired and first.token is not None, "First acquire did not return a token")
    token = first.token
    if token is None:
        raise AssertionError("An acquired claim must retain its exact token")
    holder = client.inspect(resource)
    _require(holder.held and holder.revision == token.revision, "Inspect lost the held revision")
    _require(holder.owner_label == "wheel client", "Inspect returned the wrong holder")
    raw = native.ownership_inspect(0, resource)
    _require("owner_nonce" not in raw, "Native inspect exposed an owner nonce field")
    _require(first_nonce not in json.dumps(raw), "Native reply exposed the owner nonce")
    _require(first_nonce not in repr(token), "Token representation exposed its secret")
    busy = client.try_acquire(resource, secrets.token_hex(32), "competitor", {})
    _require(not busy.acquired and busy.token is None, "A held resource admitted a competitor")
    _require(busy.holder.revision == token.revision, "Busy response changed the revision")
    client.release(token)
    second = client.try_acquire(resource, secrets.token_hex(32), "next owner", {})
    _require(second.acquired and second.token is not None, "Reacquire failed after release")
    second_token = second.token
    if second_token is None:
        raise AssertionError("Reacquisition must retain its exact token")
    _require(second_token.revision > token.revision, "Reacquire reused an old revision")
    try:
        client.release(token)
    except OwnershipConflict:
        pass
    else:
        raise AssertionError("A stale token released the newer owner")
    _require(
        client.inspect(resource).revision == second_token.revision, "Stale release changed state"
    )
    client.release(second_token)
    free = client.inspect(resource)
    _require(
        not free.held and free.revision > second_token.revision, "Release was not confirmed FREE"
    )
    print(
        "Real installed-wheel ownership RPC passed: acquire, busy, inspect, stale rejection, release"
    )


def _stop_owned_daemon(daemon: subprocess.Popen[bytes]) -> None:
    """Terminate and reap only this test's daemon, failing an unresponsive shutdown."""
    previous_status = daemon.poll()
    if previous_status is not None:
        raise RuntimeError(f"Owned runtime exited before cleanup with status {previous_status}")
    daemon.terminate()
    try:
        status = daemon.wait(timeout=10)
    except subprocess.TimeoutExpired:
        daemon.kill()
        daemon.wait(timeout=10)
        raise RuntimeError("Owned runtime required a forced kill during cleanup") from None
    _require(status in (0, -signal.SIGTERM), f"Owned runtime failed shutdown with status {status}")


def _run_smoke() -> None:
    """Own an isolated real runtime on explicitly provided persistent CI storage."""
    _require(sys.platform.startswith("linux"), "This authority smoke requires Linux")
    configured = os.environ.get("CLIO_OWNERSHIP_TEST_STORAGE_ROOT")
    _require(bool(configured), "Persistent ownership test storage must be configured explicitly")
    base = Path(configured or "")
    _require(
        base.is_absolute() and base.is_dir(),
        "Ownership test storage must be an existing absolute directory",
    )
    root = Path(tempfile.mkdtemp(prefix="ownership-wheel-", dir=base))
    _configure(root, _unused_port_cluster())
    from iowarp_core import get_bin_dir, get_lib_dir

    executable = Path(get_bin_dir()) / "clio_run"
    library_dir = Path(get_lib_dir())
    os.environ["CLIO_REPO_PATH"] = str(library_dir)
    log = root / "runtime.log"
    daemon: subprocess.Popen[bytes] | None = None
    succeeded = False
    try:
        with log.open("wb") as output:
            daemon = subprocess.Popen(
                [str(executable), "start", "--fresh", "--no-viz"],
                cwd=root,
                stdout=output,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            _wait_ready(daemon, log)
            _require_wheel_libraries(
                daemon.pid, library_dir, ("libclio_run_cxx.so", "libclio_admin_runtime.so")
            )
            subprocess.run(
                [sys.executable, str(Path(__file__).resolve()), "--client"], check=True, timeout=120
            )
            _require(daemon.poll() is None, "Owned runtime exited during the client smoke")
            _require(
                (root / "ownership-v1.ledger").is_file(), "Authority did not persist its ledger"
            )
            succeeded = True
    finally:
        try:
            if daemon is not None:
                _stop_owned_daemon(daemon)
        except (AssertionError, OSError, RuntimeError, subprocess.SubprocessError):
            succeeded = False
            raise
        finally:
            if not succeeded:
                print(f"Ownership RPC failure evidence retained at {root}", file=sys.stderr)
                if log.exists():
                    print(log.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
    shutil.rmtree(root)


if __name__ == "__main__":
    if sys.argv[1:] == ["--client"]:
        _client_smoke()
    elif not sys.argv[1:]:
        _run_smoke()
    else:
        raise RuntimeError("Unexpected ownership smoke arguments")
