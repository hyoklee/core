/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 */

// Video -> HDF5 (video_hdf5.h): the filter values for each codec, and a round
// trip of a small clip made here with FFmpeg's mpeg4 encoder. The round trip
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

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "simple_test.h"

namespace cae = clio::cae::core;

namespace {

constexpr int kW = 64, kH = 48, kFrames = 12, kFps = 10;

// Luma of frame t at (x, y): a gradient that moves right one pixel per frame.
uint8_t Pixel(int t, int x, int y) {
  return static_cast<uint8_t>(16 + ((x + t) * 3 + y) % 200);
}

// Write a kFrames-frame mpeg4 clip to `path`. Returns false if FFmpeg lacks
// the encoder or the mp4 muxer.
bool MakeClip(const std::string &path) {
  const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
  AVFormatContext *fmt = nullptr;
  if (codec == nullptr ||
      avformat_alloc_output_context2(&fmt, nullptr, "mp4", path.c_str()) < 0)
    return false;
  AVStream *st = avformat_new_stream(fmt, nullptr);
  AVCodecContext *c = avcodec_alloc_context3(codec);
  c->width = kW;
  c->height = kH;
  c->pix_fmt = AV_PIX_FMT_YUV420P;
  c->time_base = AVRational{1, kFps};
  c->framerate = AVRational{kFps, 1};
  c->gop_size = 4;
  c->bit_rate = 2000000;
  if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  bool ok = avcodec_open2(c, codec, nullptr) >= 0 &&
            avcodec_parameters_from_context(st->codecpar, c) >= 0 &&
            avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0;
  st->time_base = c->time_base;
  ok = ok && avformat_write_header(fmt, nullptr) >= 0;
  AVFrame *f = av_frame_alloc();
  AVPacket *pkt = av_packet_alloc();
  f->format = c->pix_fmt;
  f->width = kW;
  f->height = kH;
  ok = ok && av_frame_get_buffer(f, 0) >= 0;
  auto flush = [&](AVFrame *in) {
    if (avcodec_send_frame(c, in) < 0) return false;
    while (avcodec_receive_packet(c, pkt) >= 0) {
      pkt->duration = 1;  // one frame; without it the mp4 edit list drops the last
      av_packet_rescale_ts(pkt, c->time_base, st->time_base);
      pkt->stream_index = st->index;
      if (av_interleaved_write_frame(fmt, pkt) < 0) return false;
    }
    return true;
  };
  for (int t = 0; ok && t < kFrames; ++t) {
    ok = av_frame_make_writable(f) >= 0;
    for (int y = 0; y < kH; ++y)
      for (int x = 0; x < kW; ++x)
        f->data[0][y * f->linesize[0] + x] = Pixel(t, x, y);
    for (int y = 0; y < kH / 2; ++y)
      for (int x = 0; x < kW / 2; ++x) {
        f->data[1][y * f->linesize[1] + x] = 128;
        f->data[2][y * f->linesize[2] + x] = 128;
      }
    f->pts = t;
    ok = ok && flush(f);
  }
  ok = ok && flush(nullptr) && av_write_trailer(fmt) >= 0;
  av_packet_free(&pkt);
  av_frame_free(&f);
  avcodec_free_context(&c);
  if (fmt->pb) avio_closep(&fmt->pb);
  avformat_free_context(fmt);
  return ok;
}

}  // namespace

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

TEST_CASE("video_to_hdf5_bad_input", "[video]") {
  cae::VideoHdf5Options opt;
  const auto r = cae::VideoToHdf5("/nonexistent/clip.mp4", "/tmp/x.hdf5", opt);
  REQUIRE_FALSE(r.ok);
  REQUIRE(!r.error.empty());
}

SIMPLE_TEST_MAIN()
