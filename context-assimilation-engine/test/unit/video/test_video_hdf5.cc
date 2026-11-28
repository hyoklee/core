/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 */

// Video -> HDF5 (video_hdf5.h): the filter values for each codec, a round trip
// of a small clip made here with FFmpeg's mpeg4 encoder, a partial last chunk,
// and a threaded batch that must match a serial transcode. The round trip
// needs the FFMPEG HDF5 filter plugin (HDF5_PLUGIN_PATH); without it the test
// checks only that the transcode refuses with a clear message.

#include <clio_cae/core/factory/video_hdf5.h>
#include <hdf5.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <unistd.h>

#include "make_clip.h"
#include "simple_test.h"

namespace cae = clio::cae::core;
using make_clip::kFps;
using make_clip::kFrames;
using make_clip::kH;
using make_clip::kW;
using make_clip::MakeClip;
using make_clip::Pixel;

namespace {

// /frames of an HDF5 file written by VideoToHdf5, or empty if it cannot be read.
std::vector<uint8_t> ReadFrames(const std::string &h5, hsize_t dims[3]) {
  std::vector<uint8_t> px;
  hid_t file = H5Fopen(h5.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
  if (file < 0) return px;
  hid_t set = H5Dopen2(file, "frames", H5P_DEFAULT);
  hid_t space = H5Dget_space(set);
  H5Sget_simple_extent_dims(space, dims, nullptr);
  px.resize(dims[0] * dims[1] * dims[2]);
  if (H5Dread(set, H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL, H5P_DEFAULT, px.data()) < 0)
    px.clear();
  H5Sclose(space);
  H5Dclose(set);
  H5Fclose(file);
  return px;
}

std::filesystem::path TestDir(const char *name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   (std::string(name) + "_" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  return dir;
}

}  // namespace

TEST_CASE("video_hdf5_threads", "[video]") {
  hbool_t safe = 0;
  H5is_library_threadsafe(&safe);
  REQUIRE(cae::VideoHdf5Threads(4, 2) == (safe ? 2 : 1));  // never more than videos
  REQUIRE(cae::VideoHdf5Threads(0, 100) >= 1);
  REQUIRE(cae::VideoHdf5Threads(0, 100) <= 16);
  REQUIRE(cae::VideoHdf5Threads(3, 0) == 1);
}

TEST_CASE("video_filter_values", "[video]") {
  unsigned cd[11];
  std::string err;
  cae::VideoHdf5Options opt;  // x264, medium, crf 23
  REQUIRE(cae::VideoFilterValues(opt, 320, 240, 100, cd, &err));
  const unsigned want[11] = {2, 1, 320, 240, 100, 0, 15, 0, 23, 0, 0};
  for (int i = 0; i < 11; ++i) REQUIRE(cd[i] == want[i]);

  opt.codec = "x265";
  opt.preset = "slow";
  REQUIRE(cae::VideoFilterValues(opt, 2, 2, 1, cd, &err));
  REQUIRE(cd[0] == 4);
  REQUIRE(cd[1] == 3);
  REQUIRE(cd[6] == 206);

  opt.codec = "svtav1";
  opt.preset = "lessfast";
  REQUIRE(cae::VideoFilterValues(opt, 2, 2, 1, cd, &err));
  REQUIRE(cd[0] == 6);
  REQUIRE(cd[6] == 406);

  opt.preset = "lessfast";
  opt.codec = "x264";  // lessfast is an svt-av1 preset only
  REQUIRE_FALSE(cae::VideoFilterValues(opt, 2, 2, 1, cd, &err));
  REQUIRE(err.find("preset") != std::string::npos);
  opt.codec = "vp9";
  REQUIRE_FALSE(cae::VideoFilterValues(opt, 2, 2, 1, cd, &err));
  REQUIRE(err.find("codec") != std::string::npos);
  opt.codec = "x264";
  opt.preset = "fast";
  opt.crf = 64;
  REQUIRE_FALSE(cae::VideoFilterValues(opt, 2, 2, 1, cd, &err));
  REQUIRE(err.find("crf") != std::string::npos);
}

TEST_CASE("video_to_hdf5_round_trip", "[video]") {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("cae_video_test_" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  const std::string clip = (dir / "clip.mp4").string();
  const std::string h5 = (dir / "clip.mp4.hdf5").string();
  REQUIRE(MakeClip(clip));

  cae::VideoHdf5Options opt;
  opt.width = 32;  // half size: 32 x 24
  opt.crf = 10;
  const cae::VideoHdf5Result r = cae::VideoToHdf5(clip, h5, opt);
  if (!cae::VideoFilterAvailable()) {
    std::printf("  (no FFMPEG HDF5 plugin on HDF5_PLUGIN_PATH: refusal only)\n");
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error.find("HDF5_PLUGIN_PATH") != std::string::npos);
    std::filesystem::remove_all(dir);
    return;
  }
  INFO(r.error);
  REQUIRE(r.ok);
  REQUIRE(r.frames == kFrames);
  REQUIRE(r.width == 32);
  REQUIRE(r.height == 24);
  REQUIRE(r.src_width == kW);
  REQUIRE(std::fabs(r.fps - kFps) < 1e-6);

  hid_t file = H5Fopen(h5.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
  REQUIRE(file >= 0);
  hid_t set = H5Dopen2(file, "frames", H5P_DEFAULT);
  hid_t space = H5Dget_space(set);
  hsize_t dims[3] = {0, 0, 0};
  REQUIRE(H5Sget_simple_extent_ndims(space) == 3);
  H5Sget_simple_extent_dims(space, dims, nullptr);
  REQUIRE(dims[0] == kFrames);
  REQUIRE(dims[1] == 24);
  REQUIRE(dims[2] == 32);
  std::vector<uint8_t> px(dims[0] * dims[1] * dims[2]);
  REQUIRE(H5Dread(set, H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                  px.data()) >= 0);
  // Each stored pixel is the 2x2 average of the source gradient; two lossy
  // codecs (mpeg4, then x264) sit in between.
  double err_sum = 0;
  for (int t = 0; t < kFrames; ++t)
    for (int y = 0; y < 24; ++y)
      for (int x = 0; x < 32; ++x) {
        double want = 0;
        for (int dy = 0; dy < 2; ++dy)
          for (int dx = 0; dx < 2; ++dx)
            want += Pixel(t, 2 * x + dx, 2 * y + dy) / 4.0;
        err_sum += std::fabs(px[(t * 24 + y) * 32 + x] - want);
      }
  const double mean_err = err_sum / px.size();
  std::printf("  mean |stored - expected| = %.2f grey levels\n", mean_err);
  REQUIRE(mean_err < 12.0);

  hid_t tset = H5Dopen2(file, "time_s", H5P_DEFAULT);
  std::vector<double> times(kFrames);
  REQUIRE(H5Dread(tset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                  times.data()) >= 0);
  for (int t = 0; t < kFrames; ++t)
    REQUIRE(std::fabs(times[t] - static_cast<double>(t) / kFps) < 1e-6);
  H5Dclose(tset);
  H5Sclose(space);
  H5Dclose(set);
  H5Fclose(file);
  std::filesystem::remove_all(dir);
}

TEST_CASE("video_to_hdf5_partial_last_chunk", "[video]") {
  // 12 frames in chunks of 5: the last chunk holds 2 frames plus 3 of padding.
  if (!cae::VideoFilterAvailable()) return;
  const auto dir = TestDir("cae_video_chunks");
  const std::string clip = (dir / "clip.mp4").string();
  REQUIRE(MakeClip(clip));
  cae::VideoHdf5Options opt;
  opt.width = 32;
  opt.crf = 10;
  opt.frames_per_chunk = 5;
  const auto r = cae::VideoToHdf5(clip, (dir / "c.hdf5").string(), opt);
  INFO(r.error);
  REQUIRE(r.ok);
  hsize_t dims[3] = {0, 0, 0};
  const std::vector<uint8_t> px = ReadFrames((dir / "c.hdf5").string(), dims);
  REQUIRE(dims[0] == kFrames);
  REQUIRE(px.size() == static_cast<size_t>(kFrames) * 24 * 32);
  double err_sum = 0;
  for (int t = 0; t < kFrames; ++t)
    for (int y = 0; y < 24; ++y)
      for (int x = 0; x < 32; ++x) {
        double want = 0;
        for (int dy = 0; dy < 2; ++dy)
          for (int dx = 0; dx < 2; ++dx)
            want += Pixel(t, 2 * x + dx, 2 * y + dy) / 4.0;
        err_sum += std::fabs(px[(t * 24 + y) * 32 + x] - want);
      }
  REQUIRE(err_sum / px.size() < 12.0);
  std::filesystem::remove_all(dir);
}

TEST_CASE("video_to_hdf5_batch_matches_serial", "[video]") {
  // Six transcodes on 4 threads give the same frames as one on its own.
  if (!cae::VideoFilterAvailable()) return;
  const auto dir = TestDir("cae_video_batch");
  const std::string clip = (dir / "clip.mp4").string();
  REQUIRE(MakeClip(clip));
  cae::VideoHdf5Options opt;
  opt.width = 32;
  const auto solo = cae::VideoToHdf5(clip, (dir / "solo.hdf5").string(), opt);
  REQUIRE(solo.ok);
  hsize_t dims[3] = {0, 0, 0};
  const std::vector<uint8_t> want = ReadFrames((dir / "solo.hdf5").string(), dims);
  REQUIRE(!want.empty());
  std::vector<cae::VideoHdf5Job> jobs;
  for (int i = 0; i < 6; ++i)
    jobs.push_back({clip, (dir / ("b" + std::to_string(i) + ".hdf5")).string()});
  const auto res = cae::VideoToHdf5Batch(jobs, opt, 4);
  REQUIRE(res.size() == jobs.size());
  for (size_t i = 0; i < jobs.size(); ++i) {
    INFO(res[i].error);
    REQUIRE(res[i].ok);
    REQUIRE(res[i].frames == kFrames);
    hsize_t d[3] = {0, 0, 0};
    REQUIRE(ReadFrames(jobs[i].h5_path, d) == want);
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("video_to_hdf5_bad_input", "[video]") {
  cae::VideoHdf5Options opt;
  const auto r = cae::VideoToHdf5("/nonexistent/clip.mp4", "/tmp/x.hdf5", opt);
  REQUIRE_FALSE(r.ok);
  REQUIRE(!r.error.empty());
}

TEST_CASE("video_to_hdf5_source_width_and_errors", "[video]") {
  // width 0 keeps the source size; each refusal names what went wrong.
  if (!cae::VideoFilterAvailable()) return;
  const auto dir = TestDir("cae_video_errors");
  const std::string clip = (dir / "clip.mp4").string();
  REQUIRE(MakeClip(clip));
  cae::VideoHdf5Options opt;
  opt.width = 0;
  auto r = cae::VideoToHdf5(clip, (dir / "full.hdf5").string(), opt);
  INFO(r.error);
  REQUIRE(r.ok);
  REQUIRE(r.width == kW);
  REQUIRE(r.height == kH);
  REQUIRE(r.bytes_in > 0);
  REQUIRE(r.bytes_out > 0);

  opt.width = 1;  // rounds down to 0 columns
  r = cae::VideoToHdf5(clip, (dir / "tiny.hdf5").string(), opt);
  REQUIRE_FALSE(r.ok);
  REQUIRE(r.error.find("too small") != std::string::npos);

  opt.width = 32;
  opt.codec = "vp9";
  r = cae::VideoToHdf5(clip, (dir / "vp9.hdf5").string(), opt);
  REQUIRE_FALSE(r.ok);
  REQUIRE(r.error.find("codec") != std::string::npos);

  opt.codec = "x264";
  r = cae::VideoToHdf5(clip, (dir / "no_such_dir" / "x.hdf5").string(), opt);
  REQUIRE_FALSE(r.ok);
  REQUIRE(r.error.find("cannot create") != std::string::npos);

  // Not a video: FFmpeg cannot open it.
  const std::string text = (dir / "notes.mp4").string();
  {
    std::FILE *f = std::fopen(text.c_str(), "w");
    REQUIRE(f != nullptr);
    std::fputs("not a video\n", f);
    std::fclose(f);
  }
  r = cae::VideoToHdf5(text, (dir / "notes.hdf5").string(), opt);
  REQUIRE_FALSE(r.ok);
  REQUIRE(!r.error.empty());
  std::filesystem::remove_all(dir);
}

SIMPLE_TEST_MAIN()
