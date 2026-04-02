# Intel GPU (Iris Xe) Setup for WSL2 + oneAPI SYCL

## Environment

- OS: Ubuntu 24.04.3 LTS (Noble) on WSL2
- Kernel: 6.6.87.2-microsoft-standard-WSL2
- oneAPI: 2025.3.3 (icpx)
- GPU: Intel Iris Xe Graphics (visible on Windows host)

## Problem

`sycl-ls` segfaults and `test_gpu_sycl` reports no GPU device:

```
No device of requested type 'info::device_type::gpu' available.
```

`/dev/dxg` exists but `/dev/dri` is absent. The Intel GPU compute runtime
was not installed.

## Root Cause

Two issues stacked:

1. **Intel compute runtime not installed** — `/usr/lib/wsl/lib/` only contained
   D3D12 libs (`libd3d12.so`, `libdxcore.so`). The Level Zero GPU driver
   (`libze_intel_gpu.so`) was missing.

2. **Version mismatch** — Ubuntu 24.04's own repo ships `libze-intel-gpu1`
   version **23.43** (2023). oneAPI 2025.3 requires a 2025-era driver.
   Installing from Ubuntu's repo causes a segfault in `sycl-ls`.

   | Package | Ubuntu repo | Intel repo |
   |---|---|---|
   | `libze-intel-gpu1` | 23.43.27642 | 25.18.33578 |

## Fix

### 1. Add Intel's GPU apt repository

```bash
wget -qO - https://repositories.intel.com/gpu/intel-graphics.key \
  | sudo gpg --dearmor -o /usr/share/keyrings/intel-graphics.gpg

echo "deb [arch=amd64 signed-by=/usr/share/keyrings/intel-graphics.gpg] \
  https://repositories.intel.com/gpu/ubuntu noble unified" \
  | sudo tee /etc/apt/sources.list.d/intel-gpu-noble.list

sudo apt-get update
```

### 2. Install the current Intel GPU compute runtime

```bash
sudo apt-get install -y --allow-downgrades \
  libze-intel-gpu1 intel-opencl-icd libze1 libze-dev intel-level-zero-gpu
```

### 3. Verify

```bash
source /opt/intel/oneapi/setvars.sh
sycl-ls
```

Actual output after successful install:
```
[level_zero:gpu][level_zero:0] Intel(R) oneAPI Unified Runtime over Level-Zero, Intel(R) Graphics [0x46a6] 12.3.0 [1.6.33578+15]
[opencl:cpu][opencl:0] Intel(R) OpenCL, 12th Gen Intel(R) Core(TM) i7-1280P OpenCL 3.0 (Build 0) [2026.20.3.0.19_160000.xmain-hotfix]
[opencl:gpu][opencl:1] Intel(R) OpenCL Graphics, Intel(R) Graphics [0x46a6] OpenCL 3.0 NEO  [25.18.33578]
```

## Verification (Level Zero API)

Even before the driver upgrade, Level Zero itself could reach the GPU
through `/dev/dxg`:

```python
import ctypes
ze = ctypes.CDLL('libze_loader.so.1')
ze.zeInit(1)  # ZE_INIT_FLAG_GPU_ONLY

count = ctypes.c_uint32(0)
ze.zeDriverGet(ctypes.byref(count), None)
# count.value == 1  →  driver found

drivers = (ctypes.c_void_p * count.value)()
ze.zeDriverGet(ctypes.byref(count), drivers)
dev_count = ctypes.c_uint32(0)
ze.zeDeviceGet(ctypes.c_void_p(drivers[0]), ctypes.byref(dev_count), None)
# dev_count.value == 1  →  1 GPU device
```

## CTest Results (sycl-debug build)

228/230 tests pass. The 2 failures are pre-existing parallel execution races,
not SYCL-specific bugs — both pass 100% when run in isolation.

### Race condition 1: Chimaera runtime conflict

All `cte_functional_*` CTest entries run `test_core_functionality` with no
test filter, so each process executes all 9 test cases, each doing a full
Chimaera runtime init + shutdown. They compete for:

- Shared memory segment `/dev/shm/sm_segment.*`
- TCP port 9413

`RESOURCE_LOCK "chimaera_runtime"` was added to the 8 `cte_functional_*`
entries to serialize them against each other, but ~137 other tests that also
start the Chimaera runtime (`cr_*`, `cte_core_*`, etc.) have no lock and
still run concurrently.

**Affected tests:** `cte_functional_pool_creation`, `cte_functional_getblob`
(and others, non-deterministic)

**Fix needed:** Apply `RESOURCE_LOCK "chimaera_runtime"` to all ~137 tests
that start the Chimaera runtime, or pass individual test-case filters to each
CTest entry so each process only runs one test case (and thus one
init/shutdown cycle).

### Race condition 2: Socket port conflict

`socket_transport_test.cc` (test `ctp_socket_transport`) and
`socket_transport_full_test.cc` (test `ctp_socket_transport_full`) both
default to port **8193**. When they run in parallel, `bind()` fails and the
test aborts.

**Fix needed:** Change the default port in one of the two test files.

Both races were hidden in the GCC build by favorable test scheduling; the
SYCL build exposes them more consistently due to slower runtime teardown
under icpx debug compilation.

## Notes

- `/dev/dxg` is the WSL2 DirectX bridge; `/dev/dri` is not present in WSL2.
  The Intel Level Zero driver uses `/dev/dxg` via `libwsl_compute_helper.so`
  (already shipped by the Windows Intel graphics driver into
  `/usr/lib/wsl/drivers/iigd_dch.inf_amd64_*/`).
- The `ocloc` tool (needed for AOT compilation with `-fsycl-targets=spir64_gen`)
  is not available in WSL2. Use `-fsycl-targets=spir64` (JIT) instead.
  This is set via `-DSYCL_TARGET=spir64` at cmake configure time.
- The `test_gpu_sycl` test will still fail if no physical Intel GPU device
  is exposed to the WSL2 instance even after driver install — check that
  the Windows Intel graphics driver version is ≥ 31.0.101.5522.
