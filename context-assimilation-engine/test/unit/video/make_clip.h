/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 */

// A small mpeg4 clip written with FFmpeg's encoder, for the video -> HDF5
// tests (test_video_hdf5.cc and the video ingest in test_dataverse_ingest.cc).

#ifndef CAE_TEST_UNIT_VIDEO_MAKE_CLIP_H_
#define CAE_TEST_UNIT_VIDEO_MAKE_CLIP_H_

#include <cstdint>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace make_clip {

constexpr int kW = 64, kH = 48, kFrames = 12, kFps = 10;

// Luma of frame t at (x, y): a gradient that moves right one pixel per frame.
inline uint8_t Pixel(int t, int x, int y) {
  return static_cast<uint8_t>(16 + ((x + t) * 3 + y) % 200);
}

// Write a kFrames-frame mpeg4 clip to `path`. Returns false if FFmpeg lacks
// the encoder or the mp4 muxer.
inline bool MakeClip(const std::string &path) {
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

}  // namespace make_clip

#endif  // CAE_TEST_UNIT_VIDEO_MAKE_CLIP_H_
