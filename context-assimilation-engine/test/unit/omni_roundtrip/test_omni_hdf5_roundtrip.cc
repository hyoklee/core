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

/**
 * test_omni_hdf5_roundtrip.cc - OMNI-driven HDF5 assimilation round trip.
 *
 * Loads a real OMNI YAML file (CAE_OMNI_FILE), submits it through ParseOmni on
 * an in-process runtime, and then checks every HDF5 transfer against its
 * source file:
 *   - each dataset selected by dataset_filter has a tag <dst>/<dataset> whose
 *     "description" blob exists and whose chunk_N blobs concatenate to exactly
 *     the dataset's bytes (H5Dread with the file datatype);
 *   - each dataset rejected by the filter has no data in CTE.
 *
 * The test is portable (no fnmatch, no Python) so the same OMNI can be
 * exercised on Linux and Windows. Without CAE_OMNI_FILE it returns 77 (ctest
 * SKIP_RETURN_CODE) so a default ctest run is not affected.
 *
 * Environment variables:
 * - CAE_OMNI_FILE: path to the OMNI YAML to run (required, else skipped)
 * - CAE_OMNI_LAND_TIMEOUT_S: seconds to wait for blobs to land (default 180)
 */

#include <hdf5.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <clio_runtime/clio_runtime.h>
#include <clio_cae/core/core_client.h>
#include <clio_cae/core/constants.h>
#include <clio_cae/core/factory/assimilation_ctx.h>

#include <clio_cte/core/core_client.h>

#include <clio_ctp/util/logging.h>

namespace {

constexpr int kSkipReturnCode = 77;

/**
 * Split "proto::rest" into its rest part.
 * @param url URL of the form "<proto>::<rest>".
 * @return The part after "::", or "" if the separator is missing.
 */
std::string UrlPath(const std::string& url) {
  size_t pos = url.find("::");
  return pos == std::string::npos ? "" : url.substr(pos + 2);
}

/**
 * Parse an OMNI YAML file into assimilation contexts.
 * Mirrors the subset of clio_cae's loader needed for hdf5 transfers.
 * @param path OMNI YAML path.
 * @return One AssimilationCtx per entry of the "transfers" sequence.
 */
std::vector<clio::cae::core::AssimilationCtx> LoadOmni(
    const std::string& path) {
  YAML::Node config = YAML::LoadFile(path);
  std::vector<clio::cae::core::AssimilationCtx> out;
  for (const auto& t : config["transfers"]) {
    clio::cae::core::AssimilationCtx ctx;
    ctx.src = t["src"].as<std::string>();
    ctx.dst = t["dst"].as<std::string>();
    ctx.format = t["format"].as<std::string>();
    ctx.depends_on = t["depends_on"] ? t["depends_on"].as<std::string>() : "";
    ctx.range_off = t["range_off"] ? t["range_off"].as<size_t>() : 0;
    ctx.range_size = t["range_size"] ? t["range_size"].as<size_t>() : 0;
    if (t["dataset_filter"]) {
      for (const auto& p : t["dataset_filter"]["include_patterns"]) {
        ctx.include_patterns.push_back(p.as<std::string>());
      }
      for (const auto& p : t["dataset_filter"]["exclude_patterns"]) {
        ctx.exclude_patterns.push_back(p.as<std::string>());
      }
    }
    out.push_back(ctx);
  }
  return out;
}

/**
 * Match one character against a "[...]" bracket expression.
 * @param pattern Full glob pattern.
 * @param pos In: index of '['. Out: index past the expression.
 * @param c Character to test.
 * @return true on match (negation via leading '!' or '^').
 */
bool MatchBracket(const std::string& pattern, size_t& pos, char c) {
  size_t i = pos + 1;
  bool negate = false;
  if (i < pattern.size() && (pattern[i] == '!' || pattern[i] == '^')) {
    negate = true;
    ++i;
  }
  bool matched = false;
  bool first = true;
  while (i < pattern.size() && (first || pattern[i] != ']')) {
    first = false;
    char lo = pattern[i];
    char hi = lo;
    if (i + 2 < pattern.size() && pattern[i + 1] == '-' &&
        pattern[i + 2] != ']') {
      hi = pattern[i + 2];
      i += 2;
    }
    if (lo <= c && c <= hi) {
      matched = true;
    }
    ++i;
  }
  if (i >= pattern.size()) {
    pos = pos + 1;
    return c == '[';
  }
  pos = i + 1;
  return matched != negate;
}

/**
 * Portable fnmatch(pattern, str, 0) equivalent: '*' spans '/', '?' is one
 * character, "[...]" is a bracket expression, '\\' escapes.
 * @param pattern Glob pattern.
 * @param str String to test.
 * @return true if the whole string matches.
 */
bool GlobMatch(const std::string& pattern, const std::string& str) {
  const size_t npos = std::string::npos;
  size_t p = 0, s = 0, star_p = npos, star_s = 0;
  while (s < str.size()) {
    if (p < pattern.size()) {
      size_t next_p = p + 1;
      bool matched = false;
      char pc = pattern[p];
      if (pc == '*') {
        star_p = p;
        star_s = s;
        ++p;
        continue;
      } else if (pc == '?') {
        matched = true;
      } else if (pc == '[') {
        next_p = p;
        matched = MatchBracket(pattern, next_p, str[s]);
      } else {
        if (pc == '\\' && p + 1 < pattern.size()) {
          pc = pattern[p + 1];
          next_p = p + 2;
        }
        matched = (pc == str[s]);
      }
      if (matched) {
        p = next_p;
        ++s;
        continue;
      }
    }
    if (star_p == npos) {
      return false;
    }
    p = star_p + 1;
    s = ++star_s;
  }
  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

/**
 * Apply the CAE dataset_filter rules: excludes first, then includes
 * (an empty include list keeps everything not excluded).
 * @param path Dataset path with leading '/'.
 * @param ctx Transfer whose patterns apply.
 * @return true if the CAE should ingest this dataset.
 */
bool Selected(const std::string& path,
              const clio::cae::core::AssimilationCtx& ctx) {
  for (const auto& e : ctx.exclude_patterns) {
    if (GlobMatch(e, path)) {
      return false;
    }
  }
  if (ctx.include_patterns.empty()) {
    return true;
  }
  for (const auto& i : ctx.include_patterns) {
    if (GlobMatch(i, path)) {
      return true;
    }
  }
  return false;
}

/**
 * H5Lvisit callback collecting every dataset path in the file.
 */
herr_t CollectDatasets(hid_t loc_id, const char* name, const H5L_info_t*,
                       void* op_data) {
  auto* paths = static_cast<std::vector<std::string>*>(op_data);
  H5O_info_t info;
#if H5_VERSION_GE(1, 12, 0)
  herr_t st = H5Oget_info_by_name(loc_id, name, &info, H5O_INFO_BASIC,
                                  H5P_DEFAULT);
#else
  herr_t st = H5Oget_info_by_name(loc_id, name, &info, H5P_DEFAULT);
#endif
  if (st >= 0 && info.type == H5O_TYPE_DATASET) {
    paths->push_back("/" + std::string(name));
  }
  return 0;
}

/**
 * Read a whole dataset in its file datatype, exactly as the assimilator does.
 * @param file_id Open HDF5 file.
 * @param path Dataset path.
 * @param out Receives the raw bytes.
 * @return true on success.
 */
bool ReadDataset(hid_t file_id, const std::string& path,
                 std::vector<char>& out) {
  hid_t ds = H5Dopen2(file_id, path.c_str(), H5P_DEFAULT);
  if (ds < 0) {
    return false;
  }
  hid_t space = H5Dget_space(ds);
  hid_t type = H5Dget_type(ds);
  hssize_t n = H5Sget_simple_extent_npoints(space);
  size_t total = static_cast<size_t>(n) * H5Tget_size(type);
  out.resize(total);
  herr_t st = total == 0 ? 0 : H5Dread(ds, type, H5S_ALL, H5S_ALL,
                                       H5P_DEFAULT, out.data());
  H5Tclose(type);
  H5Sclose(space);
  H5Dclose(ds);
  return st >= 0;
}

/**
 * Sorted "chunk_N" blob names of a tag, plus whether "description" exists.
 */
struct TagBlobs {
  std::vector<std::string> chunks;
  bool has_description = false;
  size_t chunk_bytes = 0;
};

/**
 * List a tag's blobs and total up the chunk sizes.
 * @param cte CTE client.
 * @param tag_id Tag to inspect.
 * @return Chunk names in numeric order and the summed chunk size.
 */
TagBlobs ListTagBlobs(clio::cte::core::Client* cte,
                      const clio::cte::core::TagId& tag_id) {
  TagBlobs tb;
  auto blobs = cte->AsyncGetContainedBlobs(tag_id);
  blobs.Wait();
  for (const auto& name : blobs->blob_names_) {
    if (name == "description") {
      tb.has_description = true;
    } else if (name.rfind("chunk_", 0) == 0) {
      tb.chunks.push_back(name);
    }
  }
  std::sort(tb.chunks.begin(), tb.chunks.end(),
            [](const std::string& a, const std::string& b) {
              return std::stoul(a.substr(6)) < std::stoul(b.substr(6));
            });
  for (const auto& c : tb.chunks) {
    auto sz = cte->AsyncGetBlobSize(tag_id, c);
    sz.Wait();
    tb.chunk_bytes += sz->size_;
  }
  return tb;
}

/**
 * Read one blob of a tag through shared memory into a private buffer.
 * @return true on success; `out` receives the bytes.
 */
bool ReadBlob(clio::cte::core::Client* cte,
              const clio::cte::core::TagId& tag_id, const std::string& name,
              std::vector<char>& out) {
  auto sz = cte->AsyncGetBlobSize(tag_id, name);
  sz.Wait();
  size_t size = sz->size_;
  out.resize(size);
  if (size == 0) {
    return true;
  }
  auto buf = CLIO_IPC->AllocateBuffer(size);
  ctp::ipc::ShmPtr<> shm = buf.shm_.template Cast<void>();
  auto get = cte->AsyncGetBlob(tag_id, name, 0, size, 0, shm);
  get.Wait();
  bool ok = get->GetReturnCode() == 0;
  if (ok) {
    std::memcpy(out.data(), buf.ptr_, size);
  }
  CLIO_IPC->FreeBuffer(buf);
  return ok;
}

/**
 * Poll until the tag holds a description blob and `expected` chunk bytes.
 * @return Milliseconds waited, or -1 on timeout.
 */
double WaitLanded(clio::cte::core::Client* cte,
                  const clio::cte::core::TagId& tag_id, size_t expected,
                  double timeout_s) {
  auto t0 = std::chrono::steady_clock::now();
  while (true) {
    TagBlobs tb = ListTagBlobs(cte, tag_id);
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0)
                    .count();
    if (tb.has_description && tb.chunk_bytes == expected) {
      return ms;
    }
    if (ms > timeout_s * 1000.0) {
      return -1.0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}

/**
 * Verify one selected dataset: wait for it to land, then compare bytes.
 * @return true if the CTE copy equals the HDF5 bytes.
 */
bool VerifySelected(clio::cte::core::Client* cte, hid_t file_id,
                    const std::string& tag_prefix, const std::string& path,
                    double timeout_s) {
  std::vector<char> expected;
  if (!ReadDataset(file_id, path, expected)) {
    HLOG(kError, "  {}: H5Dread failed", path);
    return false;
  }
  auto tag = cte->AsyncGetOrCreateTag(tag_prefix + "/" + path.substr(1));
  tag.Wait();
  clio::cte::core::TagId tag_id = tag->tag_id_;
  double waited = WaitLanded(cte, tag_id, expected.size(), timeout_s);
  if (waited < 0) {
    HLOG(kError, "  {}: timed out waiting for {} bytes to land", path,
         expected.size());
    return false;
  }
  TagBlobs tb = ListTagBlobs(cte, tag_id);
  std::vector<char> desc, got, chunk;
  ReadBlob(cte, tag_id, "description", desc);
  auto t0 = std::chrono::steady_clock::now();
  for (const auto& c : tb.chunks) {
    if (!ReadBlob(cte, tag_id, c, chunk)) {
      HLOG(kError, "  {}: GetBlob failed for {}", path, c);
      return false;
    }
    got.insert(got.end(), chunk.begin(), chunk.end());
  }
  double read_ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
  bool ok = got == expected;
  std::string desc_str(desc.begin(), desc.end());
  if (ok) {
    HLOG(kSuccess,
         "  {}: desc='{}' chunks={} bytes={}/{} landed_after={} ms "
         "read={} ms -> OK",
         path, desc_str, tb.chunks.size(), got.size(), expected.size(),
         static_cast<long long>(waited), static_cast<long long>(read_ms));
  } else {
    HLOG(kError, "  {}: desc='{}' chunks={} bytes={}/{} -> FAIL (mismatch)",
         path, desc_str, tb.chunks.size(), got.size(), expected.size());
  }
  return ok;
}

/**
 * Verify one excluded dataset: its tag must hold no data.
 * @return true if nothing was ingested for it.
 */
bool VerifyExcluded(clio::cte::core::Client* cte,
                    const std::string& tag_prefix, const std::string& path) {
  auto tag = cte->AsyncGetOrCreateTag(tag_prefix + "/" + path.substr(1));
  tag.Wait();
  TagBlobs tb = ListTagBlobs(cte, tag->tag_id_);
  bool ok = tb.chunks.empty() && !tb.has_description;
  if (ok) {
    HLOG(kSuccess, "  excluded {}: 0 chunk blobs -> OK", path);
  } else {
    HLOG(kError, "  excluded {}: {} chunk blobs -> FAIL", path,
         tb.chunks.size());
  }
  return ok;
}

/**
 * Verify every dataset of one hdf5 transfer.
 * @return Number of failed datasets.
 */
int VerifyTransfer(clio::cte::core::Client* cte,
                   const clio::cae::core::AssimilationCtx& ctx,
                   double timeout_s) {
  std::string src = UrlPath(ctx.src);
  std::string prefix = UrlPath(ctx.dst);
  hid_t file_id = H5Fopen(src.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
  if (file_id < 0) {
    HLOG(kError, "Cannot open source file {}", src);
    return 1;
  }
  std::vector<std::string> paths;
  H5Lvisit(file_id, H5_INDEX_NAME, H5_ITER_NATIVE, CollectDatasets, &paths);
  int failures = 0;
  size_t selected = 0;
  for (const auto& p : paths) {
    if (Selected(p, ctx)) {
      ++selected;
      failures += VerifySelected(cte, file_id, prefix, p, timeout_s) ? 0 : 1;
    }
  }
  for (const auto& p : paths) {
    if (!Selected(p, ctx)) {
      failures += VerifyExcluded(cte, prefix, p) ? 0 : 1;
    }
  }
  H5Fclose(file_id);
  HLOG(kInfo, "[{}] {} datasets in file, {} selected, {} failure(s)", prefix,
       paths.size(), selected, failures);
  return failures;
}

/**
 * Bring up the runtime (per CLIO_WITH_RUNTIME), CTE and CAE clients, and
 * make sure the CAE pool exists.
 * @return true on success.
 */
bool InitClients() {
  if (!clio::run::CLIO_INIT(clio::run::RuntimeMode::kClient, true)) {
    HLOG(kError, "CLIO_INIT failed");
    return false;
  }
  clio::cte::core::CLIO_CTE_CLIENT_INIT();
  CLIO_CAE_CLIENT_INIT();
  clio::cae::core::Client cae_client;
  clio::cae::core::CreateParams params;
  auto create = cae_client.AsyncCreate(clio::run::PoolQuery::Local(),
                                       "cae_main",
                                       clio::cae::core::kCaePoolId, params);
  create.Wait();
  if (create->GetReturnCode() != 0) {
    HLOG(kError, "CAE pool create failed: {}", create->GetReturnCode());
    return false;
  }
  return true;
}

}  // namespace

int main(int, char**) {
  const char* omni = std::getenv("CAE_OMNI_FILE");
  if (!omni || !*omni || !std::filesystem::exists(omni)) {
    HLOG(kWarning, "CAE_OMNI_FILE not set or missing; skipping");
    return kSkipReturnCode;
  }
  const char* to = std::getenv("CAE_OMNI_LAND_TIMEOUT_S");
  double timeout_s = to && *to ? std::atof(to) : 180.0;
  if (!InitClients()) {
    return 1;
  }
  std::vector<clio::cae::core::AssimilationCtx> contexts = LoadOmni(omni);
  HLOG(kInfo, "Loaded {} transfer(s) from {}", contexts.size(), omni);
  clio::cae::core::Client cae_client(clio::cae::core::kCaePoolId);
  auto t0 = std::chrono::steady_clock::now();
  auto parse = cae_client.AsyncParseOmni(contexts);
  parse.Wait();
  double parse_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
  HLOG(kInfo, "ParseOmni rc={} scheduled={} in {} ms",
       parse->GetReturnCode(), parse->num_tasks_scheduled_,
       static_cast<long long>(parse_ms));
  if (parse->GetReturnCode() != 0) {
    return 1;
  }
  auto* cte = CLIO_CTE_CLIENT;
  int failures = 0;
  for (const auto& ctx : contexts) {
    if (ctx.format != "hdf5") {
      HLOG(kWarning, "Skipping non-hdf5 transfer {}", ctx.src);
      continue;
    }
    failures += VerifyTransfer(cte, ctx, timeout_s);
  }
  if (failures == 0) {
    HLOG(kSuccess, "OMNI HDF5 ROUND TRIP PASSED");
    return 0;
  }
  HLOG(kError, "OMNI HDF5 ROUND TRIP FAILED ({} dataset(s))", failures);
  return 1;
}
