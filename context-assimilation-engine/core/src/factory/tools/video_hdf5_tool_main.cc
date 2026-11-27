/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 */

// cae_video_hdf5: transcode one video into HDF5 the way an OMNI `video:` block
// does, without a runtime.
//
//   cae_video_hdf5 <video> <out.h5> [--width N] [--codec x264|x265|svtav1]
//                  [--crf N] [--preset NAME] [--frames-per-chunk N]
//
// HDF5_PLUGIN_PATH must name the directory of libh5ffmpeg_shared.so.

#include <clio_cae/core/factory/video_hdf5.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv) {
  using clio::cae::core::VideoHdf5Options;
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s <video> <out.h5> [--width N] [--codec "
                 "x264|x265|svtav1] [--crf N] [--preset NAME] "
                 "[--frames-per-chunk N]\n",
                 argv[0]);
    return 2;
  }
  VideoHdf5Options opt;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (i + 1 >= argc) {
      std::fprintf(stderr, "%s needs a value\n", a.c_str());
      return 2;
    }
    const char *v = argv[++i];
    if (a == "--width") {
      opt.width = std::atoi(v);
    } else if (a == "--codec") {
      opt.codec = v;
    } else if (a == "--crf") {
      opt.crf = std::atoi(v);
    } else if (a == "--preset") {
      opt.preset = v;
    } else if (a == "--frames-per-chunk") {
      opt.frames_per_chunk = std::atoi(v);
    } else {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      return 2;
    }
  }
  const auto r = clio::cae::core::VideoToHdf5(argv[1], argv[2], opt);
  if (!r.ok) {
    std::fprintf(stderr, "cae_video_hdf5: %s\n", r.error.c_str());
    return 1;
  }
  std::printf(
      "%s -> %s: %lld frames %dx%d (source %dx%d, %.3f fps), %zu -> %zu "
      "bytes\n",
      argv[1], argv[2], static_cast<long long>(r.frames), r.width, r.height,
      r.src_width, r.src_height, r.fps, r.bytes_in, r.bytes_out);
  return 0;
}
