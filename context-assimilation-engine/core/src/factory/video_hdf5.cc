/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 */

#include <clio_cae/core/factory/video_hdf5.h>

#include <dlfcn.h>
#include <hdf5.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace clio::cae::core {

namespace {

std::string AvError(int rc) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(rc, buf, sizeof(buf));
  return buf;
}

// Ids from ffmpeg_HDF5_filter (h5ffmpeg/constants.py): encoder, decoder, and
// the first preset id of each encoder's preset list.
struct CodecIds {
  const char *name;
  unsigned encoder, decoder, preset_base;
  const char *const *presets;
};
const char *const kX26xPresets[] = {"ultrafast", "superfast", "veryfast",
                                    "faster",    "fast",      "medium",
                                    "slow",      "slower",    "veryslow",
                                    nullptr};
const char *const kSvtPresets[] = {
    "ultrafast", "superfast", "veryfast", "muchfaster", "faster",
    "fast",      "lessfast",  "medium",   "lessslow",   "slow",
    "slower",    "muchslower", "veryslow", "superslow", nullptr};
const CodecIds kCodecs[] = {
    {"x264", 2, 1, 10, kX26xPresets},
    {"x265", 4, 3, 200, kX26xPresets},
    {"svtav1", 6, 6, 400, kSvtPresets},
};

// RAII holders for the libav and HDF5 handles.
struct Decoder {
  AVFormatContext *fmt = nullptr;
  AVCodecContext *dec = nullptr;
  SwsContext *sws = nullptr;
  AVPacket *pkt = nullptr;
  AVFrame *frame = nullptr;
  ~Decoder() {
    av_frame_free(&frame);
    av_packet_free(&pkt);
    sws_freeContext(sws);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
  }
};
struct H5Ids {
  std::vector<hid_t> files, sets, spaces, plists, attrs, types;
  void Close() {
    for (hid_t h : attrs) H5Aclose(h);
    for (hid_t h : sets) H5Dclose(h);
    for (hid_t h : spaces) H5Sclose(h);
    for (hid_t h : plists) H5Pclose(h);
    for (hid_t h : types) H5Tclose(h);
    for (hid_t h : files) H5Fclose(h);
    attrs.clear(), sets.clear(), spaces.clear(), plists.clear(), types.clear(),
        files.clear();
  }
  ~H5Ids() { Close(); }
};

bool WriteStringAttr(H5Ids &ids, hid_t obj, const char *name,
                     const std::string &value) {
  hid_t type = H5Tcopy(H5T_C_S1);
  ids.types.push_back(type);
  H5Tset_size(type, std::max<size_t>(1, value.size()));
  hid_t space = H5Screate(H5S_SCALAR);
  ids.spaces.push_back(space);
  hid_t attr = H5Acreate2(obj, name, type, space, H5P_DEFAULT, H5P_DEFAULT);
  if (attr < 0) return false;
  ids.attrs.push_back(attr);
  return H5Awrite(attr, type, value.c_str()) >= 0;
}

bool WriteScalarAttr(H5Ids &ids, hid_t obj, const char *name, hid_t mem_type,
                     const void *value) {
  hid_t space = H5Screate(H5S_SCALAR);
  ids.spaces.push_back(space);
  hid_t attr = H5Acreate2(obj, name, mem_type, space, H5P_DEFAULT, H5P_DEFAULT);
  if (attr < 0) return false;
  ids.attrs.push_back(attr);
  return H5Awrite(attr, mem_type, value) >= 0;
}

// The FFMPEG filter's function (H5Z_func_t), taken from the plugin library so
// worker threads can encode chunks outside HDF5: with a thread-safe HDF5 every
// H5Dwrite runs under one global lock, and an encode inside it would serialize
// all transcodes. The function keeps no global state (one codec context per
// call) and allocates with H5allocate_memory, as HDF5 expects.
using FilterFn = size_t (*)(unsigned, size_t, const unsigned[], size_t, size_t *,
                            void **);

FilterFn LoadFilterFn(std::string *error) {
  static std::once_flag once;
  static FilterFn fn = nullptr;
  static std::string why;
  std::call_once(once, [] {
    if (H5Zfilter_avail(static_cast<H5Z_filter_t>(kFfmpegH5FilterId)) <= 0) {
      why = "the FFMPEG HDF5 filter (id 32030) is not available: set "
            "HDF5_PLUGIN_PATH to the directory of libh5ffmpeg_shared.so";
      return;
    }
    // HDF5 has loaded the plugin; find the same library on its search path.
    unsigned n = 0;
    H5PLsize(&n);
    for (unsigned i = 0; i < n && fn == nullptr; ++i) {
      char dir[4096] = {0};
      if (H5PLget(i, dir, sizeof(dir)) <= 0) continue;
      std::error_code ec;
      for (const auto &e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.find("ffmpeg") == std::string::npos ||
            e.path().extension() != ".so")
          continue;
        void *lib = dlopen(e.path().c_str(), RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr) continue;
        auto type = reinterpret_cast<H5PL_type_t (*)()>(
            dlsym(lib, "H5PLget_plugin_type"));
        auto info = reinterpret_cast<const void *(*)()>(
            dlsym(lib, "H5PLget_plugin_info"));
        const auto *cls =
            info != nullptr ? static_cast<const H5Z_class2_t *>(info()) : nullptr;
        if (type != nullptr && type() == H5PL_TYPE_FILTER && cls != nullptr &&
            cls->id == static_cast<H5Z_filter_t>(kFfmpegH5FilterId) &&
            cls->filter != nullptr) {
          fn = reinterpret_cast<FilterFn>(cls->filter);
          break;  // keep the library open for the life of the process
        }
        dlclose(lib);
      }
    }
    if (fn == nullptr)
      why = "the FFMPEG filter is loadable by HDF5, but no *ffmpeg*.so on the "
            "HDF5 plugin path exports it";
  });
  if (fn == nullptr && error != nullptr) *error = why;
  return fn;
}

// Run the filter forward on one chunk: `nbytes` of frames in, encoded bytes out.
bool EncodeChunk(FilterFn fn, const unsigned cd[11], const uint8_t *src,
                 size_t nbytes, std::vector<uint8_t> *out) {
  void *buf = H5allocate_memory(nbytes, false);
  if (buf == nullptr) return false;
  std::memcpy(buf, src, nbytes);
  size_t buf_size = nbytes;
  const size_t n = fn(0, 11, cd, nbytes, &buf_size, &buf);
  if (n > 0)
    out->assign(static_cast<uint8_t *>(buf), static_cast<uint8_t *>(buf) + n);
  H5free_memory(buf);
  return n > 0;
}

}  // namespace

bool VideoFilterValues(const VideoHdf5Options &opt, int width, int height,
                       int depth, unsigned cd_values[11], std::string *error) {
  const CodecIds *codec = nullptr;
  for (const auto &c : kCodecs)
    if (opt.codec == c.name) codec = &c;
  if (codec == nullptr) {
    *error = "unknown video codec '" + opt.codec + "' (x264, x265, svtav1)";
    return false;
  }
  int preset = -1;
  for (int i = 0; codec->presets[i] != nullptr; ++i)
    if (opt.preset == codec->presets[i]) preset = i;
  if (preset < 0) {
    *error = "unknown preset '" + opt.preset + "' for " + opt.codec;
    return false;
  }
  if (opt.crf < 0 || opt.crf > 63) {
    *error = "crf must be 0..63";
    return false;
  }
  cd_values[0] = codec->encoder;
  cd_values[1] = codec->decoder;
  cd_values[2] = static_cast<unsigned>(width);
  cd_values[3] = static_cast<unsigned>(height);
  cd_values[4] = static_cast<unsigned>(depth);
  cd_values[5] = 0;  // 8-bit
  cd_values[6] = codec->preset_base + static_cast<unsigned>(preset);
  cd_values[7] = 0;  // no tune
  cd_values[8] = static_cast<unsigned>(opt.crf);
  cd_values[9] = 0;   // film grain (svt-av1 only)
  cd_values[10] = 0;  // gpu id (nvenc only)
  return true;
}

bool VideoFilterAvailable() {
  return H5Zfilter_avail(static_cast<H5Z_filter_t>(kFfmpegH5FilterId)) > 0;
}

VideoHdf5Result VideoToHdf5(const std::string &video_path,
                            const std::string &h5_path,
                            const VideoHdf5Options &opt) {
  VideoHdf5Result r;
  std::error_code ec;
  r.bytes_in = std::filesystem::file_size(video_path, ec);
  const FilterFn filter = LoadFilterFn(&r.error);
  if (filter == nullptr) return r;

  // Decode every frame to 8-bit luma at the output size.
  Decoder d;
  int rc = avformat_open_input(&d.fmt, video_path.c_str(), nullptr, nullptr);
  if (rc < 0) {
    r.error = "cannot open " + video_path + ": " + AvError(rc);
    return r;
  }
  if ((rc = avformat_find_stream_info(d.fmt, nullptr)) < 0) {
    r.error = "no stream info in " + video_path + ": " + AvError(rc);
    return r;
  }
  const AVCodec *codec = nullptr;
  const int si =
      av_find_best_stream(d.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  if (si < 0 || codec == nullptr) {
    r.error = "no decodable video stream in " + video_path;
    return r;
  }
  AVStream *st = d.fmt->streams[si];
  d.dec = avcodec_alloc_context3(codec);
  if (d.dec == nullptr ||
      avcodec_parameters_to_context(d.dec, st->codecpar) < 0 ||
      (rc = avcodec_open2(d.dec, codec, nullptr)) < 0) {
    r.error = "cannot open the " + std::string(codec->name) + " decoder";
    return r;
  }
  r.src_width = d.dec->width;
  r.src_height = d.dec->height;
  if (r.src_width <= 0 || r.src_height <= 0) {
    r.error = "video has no frame size";
    return r;
  }
  int w = opt.width > 0 ? std::min(opt.width, r.src_width) : r.src_width;
  int h = static_cast<int>(static_cast<int64_t>(r.src_height) * w /
                           r.src_width);
  w &= ~1;  // 4:2:0 needs even sizes
  h &= ~1;
  if (w < 2 || h < 2) {
    r.error = "output frame size too small";
    return r;
  }
  r.width = w;
  r.height = h;
  const AVRational fr = av_guess_frame_rate(d.fmt, st, nullptr);
  r.fps = fr.den > 0 ? av_q2d(fr) : 0;
  const double tb = av_q2d(st->time_base);

  d.pkt = av_packet_alloc();
  d.frame = av_frame_alloc();
  std::vector<uint8_t> frames;
  std::vector<double> times;
  const size_t plane = static_cast<size_t>(w) * h;
  auto take = [&](AVFrame *f) -> bool {
    d.sws = sws_getCachedContext(d.sws, f->width, f->height,
                                 static_cast<AVPixelFormat>(f->format), w, h,
                                 AV_PIX_FMT_GRAY8, SWS_AREA, nullptr, nullptr,
                                 nullptr);
    if (d.sws == nullptr) return false;
    frames.resize(frames.size() + plane);
    uint8_t *dst[4] = {frames.data() + frames.size() - plane, nullptr, nullptr,
                       nullptr};
    int dst_stride[4] = {w, 0, 0, 0};
    sws_scale(d.sws, f->data, f->linesize, 0, f->height, dst, dst_stride);
    const int64_t pts = f->best_effort_timestamp;
    times.push_back(pts == AV_NOPTS_VALUE
                        ? (r.fps > 0 ? times.size() / r.fps : 0.0)
                        : pts * tb);
    return true;
  };
  auto drain = [&]() -> int {
    int ret;
    while ((ret = avcodec_receive_frame(d.dec, d.frame)) >= 0) {
      if (!take(d.frame)) return AVERROR(EINVAL);
      av_frame_unref(d.frame);
    }
    return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : ret;
  };
  while ((rc = av_read_frame(d.fmt, d.pkt)) >= 0) {
    if (d.pkt->stream_index == si) {
      rc = avcodec_send_packet(d.dec, d.pkt);
      if (rc >= 0) rc = drain();
    }
    av_packet_unref(d.pkt);
    if (rc < 0) {
      r.error = "decoding " + video_path + " failed: " + AvError(rc);
      return r;
    }
  }
  avcodec_send_packet(d.dec, nullptr);  // flush
  if ((rc = drain()) < 0) {
    r.error = "decoding " + video_path + " failed: " + AvError(rc);
    return r;
  }
  r.frames = static_cast<int64_t>(times.size());
  if (r.frames == 0) {
    r.error = "no frames decoded from " + video_path;
    return r;
  }

  // Encode each chunk here, outside HDF5 (see LoadFilterFn). The last chunk is
  // padded with zero frames, as HDF5 pads a partial chunk with the fill value
  // before filtering; a reader gets back only the first T frames.
  const int depth =
      static_cast<int>(std::min<int64_t>(r.frames, std::max(1, opt.frames_per_chunk)));
  unsigned cd[11];
  if (!VideoFilterValues(opt, w, h, depth, cd, &r.error)) return r;
  const size_t chunk_bytes = plane * depth;
  const size_t n_chunks = (r.frames + depth - 1) / depth;
  frames.resize(n_chunks * chunk_bytes, 0);
  std::vector<std::vector<uint8_t>> encoded(n_chunks);
  for (size_t c = 0; c < n_chunks; ++c) {
    if (!EncodeChunk(filter, cd, frames.data() + c * chunk_bytes, chunk_bytes,
                     &encoded[c])) {
      r.error = "encoding /frames chunk " + std::to_string(c) +
                " with the FFMPEG filter failed";
      return r;
    }
  }

  // Write /frames (chunks already filtered) and /time_s.
  H5Ids ids;
  hid_t file = H5Fcreate(h5_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  if (file < 0) {
    r.error = "cannot create " + h5_path;
    return r;
  }
  ids.files.push_back(file);
  const hsize_t dims[3] = {static_cast<hsize_t>(r.frames),
                           static_cast<hsize_t>(h), static_cast<hsize_t>(w)};
  const hsize_t chunk[3] = {static_cast<hsize_t>(depth), dims[1], dims[2]};
  hid_t space = H5Screate_simple(3, dims, nullptr);
  ids.spaces.push_back(space);
  hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
  ids.plists.push_back(dcpl);
  if (H5Pset_chunk(dcpl, 3, chunk) < 0 ||
      H5Pset_filter(dcpl, static_cast<H5Z_filter_t>(kFfmpegH5FilterId),
                    H5Z_FLAG_MANDATORY, 11, cd) < 0) {
    r.error = "cannot set the FFMPEG filter on /frames";
    return r;
  }
  hid_t set = H5Dcreate2(file, "frames", H5T_NATIVE_UINT8, space, H5P_DEFAULT,
                         dcpl, H5P_DEFAULT);
  if (set < 0) {
    r.error = "cannot create /frames";
    return r;
  }
  ids.sets.push_back(set);
  for (size_t c = 0; c < n_chunks; ++c) {
    const hsize_t offset[3] = {c * static_cast<hsize_t>(depth), 0, 0};
    if (H5Dwrite_chunk(set, H5P_DEFAULT, /*filter_mask=*/0, offset,
                       encoded[c].size(), encoded[c].data()) < 0) {
      r.error = "writing /frames chunk " + std::to_string(c) + " failed";
      return r;
    }
  }
  const int32_t sw = r.src_width, sh = r.src_height;
  if (!WriteStringAttr(ids, set, "source",
                       std::filesystem::path(video_path).filename().string()) ||
      !WriteStringAttr(ids, set, "codec", opt.codec) ||
      !WriteStringAttr(ids, set, "pixels", "luma (8-bit gray)") ||
      !WriteScalarAttr(ids, set, "src_width", H5T_NATIVE_INT32, &sw) ||
      !WriteScalarAttr(ids, set, "src_height", H5T_NATIVE_INT32, &sh) ||
      !WriteScalarAttr(ids, set, "fps", H5T_NATIVE_DOUBLE, &r.fps)) {
    r.error = "cannot write /frames attributes";
    return r;
  }
  const hsize_t tdims[1] = {static_cast<hsize_t>(r.frames)};
  hid_t tspace = H5Screate_simple(1, tdims, nullptr);
  ids.spaces.push_back(tspace);
  hid_t tset = H5Dcreate2(file, "time_s", H5T_NATIVE_DOUBLE, tspace,
                          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (tset < 0 || (ids.sets.push_back(tset),
                   H5Dwrite(tset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                            H5P_DEFAULT, times.data()) < 0)) {
    r.error = "cannot write /time_s";
    return r;
  }
  if (H5Fflush(file, H5F_SCOPE_GLOBAL) < 0) {
    r.error = "cannot flush " + h5_path;
    return r;
  }
  ids.Close();  // close everything before the size check
  r.bytes_out = std::filesystem::file_size(h5_path, ec);
  r.ok = true;
  return r;
}

int VideoHdf5Threads(int requested, size_t n_videos) {
  hbool_t safe = 0;
  if (H5is_library_threadsafe(&safe) < 0 || !safe) return 1;
  int n = requested;
  if (n <= 0) {
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    n = std::clamp(hw / 2, 1, 16);
  }
  return std::max(1, std::min<int>(n, static_cast<int>(std::max<size_t>(1, n_videos))));
}

std::vector<VideoHdf5Result> VideoToHdf5Batch(const std::vector<VideoHdf5Job> &jobs,
                                              const VideoHdf5Options &opt,
                                              int threads) {
  std::vector<VideoHdf5Result> out(jobs.size());
  const int n = VideoHdf5Threads(threads, jobs.size());
  std::atomic<size_t> next{0};
  auto worker = [&] {
    for (size_t i; (i = next.fetch_add(1)) < jobs.size();)
      out[i] = VideoToHdf5(jobs[i].video_path, jobs[i].h5_path, opt);
  };
  std::vector<std::thread> pool;
  for (int t = 1; t < n; ++t) pool.emplace_back(worker);
  worker();
  for (auto &t : pool) t.join();
  return out;
}

}  // namespace clio::cae::core
