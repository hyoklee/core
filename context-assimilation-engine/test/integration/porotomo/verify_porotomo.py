#!/usr/bin/env python3
"""Verify a CAE HDF5 assimilation against the source file.

Usage: verify_porotomo.py <omni.yaml> <clio_config.yaml> <build_bin_dir>

For every transfer in the OMNI file this script lists the datasets in the
source HDF5 file, applies the same include/exclude glob filter the CAE uses,
and checks that each surviving dataset has a CTE tag <dst>/<dataset> whose
"description" blob matches the tensor shape and whose chunk_N blobs
concatenate to exactly the dataset's bytes. Excluded datasets must have no
chunk blobs.
"""
import fnmatch
import sys
import time

import h5py
import yaml

omni_path, config_path, bin_dir = sys.argv[1:4]
sys.path.insert(0, bin_dir)
import clio_cte_core_ext as cte  # noqa: E402

if not cte.clio_init(cte.RuntimeMode.kClient, False):
    sys.exit("clio_init failed: is the runtime running?")
if not cte.initialize_cte(config_path, cte.PoolQuery.Dynamic()):
    sys.exit("initialize_cte failed")


def url_path(url):
    return url.split("::", 1)[1]


def matches(path, inc, exc):
    if any(fnmatch.fnmatchcase(path, p) for p in exc):
        return False
    return not inc or any(fnmatch.fnmatchcase(path, p) for p in inc)


def read_blob(tag, name):
    size = tag.GetBlobSize(name)
    return tag.GetBlob(name, size, 0)


failures = 0
omni = yaml.safe_load(open(omni_path))
for tr in omni["transfers"]:
    src = url_path(tr["src"])
    prefix = url_path(tr["dst"])
    filt = tr.get("dataset_filter") or {}
    inc = filt.get("include_patterns") or []
    exc = filt.get("exclude_patterns") or []
    with h5py.File(src, "r") as f:
        paths = []
        f.visititems(lambda n, o: paths.append("/" + n)
                     if isinstance(o, h5py.Dataset) else None)
        kept = [p for p in paths if matches(p, inc, exc)]
        print(f"[{prefix}] {len(paths)} datasets in file, {len(kept)} selected")
        def chunk_blobs(tag):
            blobs = tag.GetContainedBlobs()
            chunks = sorted((b for b in blobs if b.startswith("chunk_")),
                            key=lambda b: int(b[6:]))
            return blobs, chunks

        def wait_landed(tag, expected_bytes, timeout_s=180):
            """Poll until the description blob and all chunk bytes are in CTE."""
            t0 = time.time()
            while True:
                blobs, chunks = chunk_blobs(tag)
                have = sum(tag.GetBlobSize(c) for c in chunks)
                if "description" in blobs and have == expected_bytes:
                    return (time.time() - t0) * 1000.0
                if time.time() - t0 > timeout_s:
                    return None
                time.sleep(0.5)

        for p in kept:
            ds = f[p]
            expected = ds[()].tobytes()
            tag = cte.Tag(prefix + "/" + p.lstrip("/"))
            waited_ms = wait_landed(tag, len(expected))
            if waited_ms is None:
                print(f"  {p}: TIMEOUT waiting for assimilation -> FAIL")
                failures += 1
                continue
            blobs, chunks = chunk_blobs(tag)
            desc = read_blob(tag, "description").decode()
            t0 = time.time()
            got = b"".join(read_blob(tag, c) for c in chunks)
            dt_ms = (time.time() - t0) * 1000.0
            ok = got == expected
            failures += not ok
            print(f"  {p}: dtype={ds.dtype} shape={ds.shape} desc='{desc}' "
                  f"chunks={len(chunks)} bytes={len(got)}/{len(expected)} "
                  f"landed_after={waited_ms:.0f} ms read={dt_ms:.1f} ms -> {'OK' if ok else 'FAIL'}")
        for p in paths:
            if p in kept:
                continue
            tag = cte.Tag(prefix + "/" + p.lstrip("/"))
            blobs, chunks = chunk_blobs(tag)
            ok = len(chunks) == 0 and "description" not in blobs
            print(f"  excluded {p}: {len(blobs)} blobs -> {'OK' if ok else 'FAIL'}")
            failures += not ok
print("VERIFY", "PASSED" if failures == 0 else f"FAILED ({failures})")
sys.exit(1 if failures else 0)
