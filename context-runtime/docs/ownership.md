# Fixed-authority, non-expiring ownership

This source feature adds a native Admin coordination service and the public
`iowarp_core.ownership` Python client. It is awaiting focused CI qualification
and a core release. Installing or running an existing 2.3.1 wheel does not add
this capability.

Every participating writer uses the same physical core authority, persistent
authority UUID, and stable resource identity. The authority serializes acquire,
release, and inspect requests. Ownership never expires. A paused or disconnected
owner therefore cannot be replaced by a second writer merely because time
passes. There is no automatic steal, authority failover, or force-unlock API.

## Configure the authority

Use one designated Linux core runtime, with its existing hostfile assigning a
fixed unsigned 32-bit node ID; the default single-node runtime uses node 0.
Configure that daemon before starting it:

```sh
export CLIO_OWNERSHIP_AUTHORITY_NODE=0
export CLIO_OWNERSHIP_STORAGE_ROOT=/var/lib/clio/ownership
```

Create the storage directory as private, persistent, local storage belonging to
the daemon's OS user. Preserve it across restarts. It must not be an NFS-mounted
home directory, a shared blueprint directory, an ephemeral container layer, or
volatile memory storage. There is deliberately no default under `$HOME` or
`CLIO_STORAGE_ROOT`.

The authority stores `ownership-v1.ledger` there. Its process-exclusive file
handle prevents two local daemon instances from writing the same ledger. It is
not the distributed lock: clients coordinate through the designated core RPC
service. Known NFS, SMB/CIFS, SMB2, tmpfs, and ramfs types are rejected.
Other types require operator qualification as persistent local storage;
filesystem type detection cannot prove a storage system's durability guarantees.

Windows and macOS clients may contact the Linux authority. An authority on
either client platform fails closed because its local-filesystem qualification
and directory durability have not been established. Do not bypass this check.

## Bind clients explicitly

Initialize core in client-only mode against the authority's configured hostfile.
Set configuration before importing or initializing core. Do not use a private
automatically hosted runtime as a fallback.

```python
import os

os.environ["CLIO_WITH_RUNTIME"] = "0"
os.environ["CLIO_SERVER_CONF"] = "/etc/clio/shared-runtime.yaml"

from iowarp_core import clio_cte_core_ext
from iowarp_core.ownership import OwnershipClient

if not clio_cte_core_ext.clio_init(clio_cte_core_ext.RuntimeMode.kClient, False):
    raise RuntimeError("Could not attach to the configured shared core runtime")

# Setup only: an operator records and distributes this persistent UUID.
identity = OwnershipClient.inspect_authority(0)
print(identity.authority_uuid)
```

Persist the inspected UUID in deployment configuration. An application's normal
startup must require that configured value. It must not inspect and silently
adopt whichever UUID currently answers. Deleting or recreating the ledger gives
the authority a different UUID; already bound clients then fail closed.

```python
client = OwnershipClient(
    authority_node_id=0,
    expected_authority_uuid=os.environ["CLIO_BLUEPRINT_AUTHORITY_UUID"],
)
```

Requests use physical-node routing and the server checks its configured node
identity. They do not use logical-container successor routing. An unavailable,
uninitialized, misconfigured, or incompatible authority is an error, not
permission to use an OS lock or unconditional blob writes instead.

Every client must use the same hostfile and physical-node mapping. There must be
exactly one active authority ledger instance. A copied ledger has the same UUID;
the UUID alone cannot prevent two machines from serving that copy independently.
Do not clone or simultaneously serve this state, and do not add failover or
automatic migration to this single-authority protocol.

An application that already initialized this native core client, including a
`ClioCoreStore` process, reuses that attachment. The ownership API checks the
existing IPC manager and does not reinitialize or start a transport. Its authority
node must be reachable through the hostfile mapping used for the original attach.
Configure the authority daemon before it starts; setting authority environment
variables only in an already attached client does not configure that daemon.

## Acquire, edit, and explicitly release

Resource keys are stable identities shared by all writers, for example
`blueprint-store:<persisted-store-uuid>`. Mount paths are not identities: two
agents may mount the same store at different paths.

```python
import secrets
import socket

nonce = secrets.token_hex(32)
# Retain this nonce securely before the request, including across uncertain RPCs.
result = client.try_acquire(
    "blueprint-store:6e92cb61-26b2-41f1-b29d-1c11df40c176",
    owner_nonce=nonce,
    owner_label="marketplace installer",
    holder_metadata={"hostname": socket.gethostname(), "pid": os.getpid()},
)
if not result.acquired:
    raise RuntimeError(f"Blueprint store is held by {result.holder.owner_label}")

token = result.token
assert token is not None
# Retain the exact token securely until the operation has ended.
# Perform the real store mutation here; every writer must use this protocol.
client.release(token)
```

The native service does not perform arbitrary blueprint filesystem mutations.
All mutation paths must participate, including install, update, uninstall,
repair, import, and activation. Publishing after a caller has released its token
violates this protocol. A non-expiring owner removes the lease-expiry race; it
does not make an uncooperative direct filesystem writer safe.

Use a new cryptographically random 64-character lowercase hexadecimal nonce
for each acquisition attempt. If an acquisition RPC result is uncertain, retry
with the **same nonce**, resource, and configured authority. A current-owner
retry returns the existing ownership revision without appending a new record.
A competing nonce returns `acquired=False` and the public holder information.

Release requires the exact authority UUID, resource, held revision, and nonce.
It appends a durable FREE tombstone with a new integer revision. Stale tokens,
wrong nonces, wrong authorities, and already free resources are rejected.
Native replies and inspect never include the nonce; Python token representation
also hides it. Holder labels and metadata are public: never put credentials or
nonces in them. Core RPC transport must be restricted to trusted participants.

## Failures and operator recovery

The ledger writes a framed, checksummed record and completes its file durability
barrier before reporting success. Initial authority creation also completes a
parent-directory durability barrier. Revisions are persisted integers, not wall
clock timestamps. A clean runtime restart preserves HELD ownership and its
revision. There are no timer-based recovery rules.

An append or sync failure poisons that service instance. A corrupt, truncated,
empty existing, or inconsistent ledger fails recovery; it is not truncated,
repaired, reset, or treated as FREE automatically. Resolve the storage failure
and preserve evidence before restarting. An uncertain successful disk write
may have established ownership even when its caller received an error.

The Python binding releases the GIL while waiting for an RPC. It requires a real
completed response and fails closed on a missing task, transport failure,
authority error, or incomplete response. An RPC deadline is not an ownership
deadline and must never be interpreted as releasing or stealing the resource.

After a writer crashes, identify it using the public hostname, PID, process-start
identity, and agent label where provided. A reused PID alone is insufficient.
Before explicitly releasing its retained exact token, prove that the former
writer and all children capable of publishing have stopped. The API cannot prove
that fact for the operator. If the exact token is lost, ownership remains HELD:
there is no supported force release or delete-the-ledger recovery shortcut.

Replacing an authority ledger or explicitly rebinding a changed UUID requires
all potential writers to be quiesced. Never silently serve a stale backup as the
same authority: that can discard held claims or roll revisions backward.

This design provides one fixed durable authority. It does not provide replicated
consensus, high availability, automatic authority replacement, or protection
against privileged users directly editing the ledger or shared resource.
