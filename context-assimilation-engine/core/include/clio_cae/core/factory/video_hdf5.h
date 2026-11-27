/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 */

#ifndef CLIO_CAE_CORE_FACTORY_VIDEO_HDF5_H_
#define CLIO_CAE_CORE_FACTORY_VIDEO_HDF5_H_

/**
 * Video -> HDF5 transcoding for CAE (OMNI `video:` block).
 *
 * A video file (anything libavformat opens: mp4, avi, mkv, ...) becomes an
 * HDF5 file holding
 *   /frames  uint8 [T, H, W]  8-bit luma, compressed with the FFMPEG HDF5
 *                              filter (id 32030, Cai Lab ffmpeg_HDF5_filter)
 *   /time_s  float64 [T]      presentation time of each frame
 * and attributes on /frames: source, codec, src_width, src_height, fps.
 *
 * The filter is a dynamically loaded HDF5 plugin: HDF5_PLUGIN_PATH must name
 * the directory of libh5ffmpeg_shared.so, built against the same HDF5 as this
 * library. The filter has no colour mode, so frames are stored as luma.
 */

#include <cstddef>
#include <cstdint>
#include <string>

namespace clio::cae::core {

/** FFMPEG HDF5 filter id (registered with The HDF Group). */
constexpr unsigned kFfmpegH5FilterId = 32030;

/** How a video is transcoded; the OMNI `video:` block fills it. */
struct VideoHdf5Options {
  /** Output width in pixels; 0 keeps the source width. The height follows the
   *  source aspect ratio. Both are rounded down to even numbers (4:2:0). */
  int width = 0;
  /** Encoder for the HDF5 filter: "x264", "x265" or "svtav1". */
  std::string codec = "x264";
  /** Constant rate factor (lower = higher quality). */
  int crf = 23;
  /** Encoder preset: ultrafast ... veryslow (svtav1: ultrafast ... superslow). */
  std::string preset = "medium";
  /** At most this many frames per HDF5 chunk (one encoded clip per chunk). */
  int frames_per_chunk = 512;
};

/** What a transcode produced. */
struct VideoHdf5Result {
  bool ok = false;
  std::string error;
  int64_t frames = 0;
  int width = 0, height = 0;          // stored frame size
  int src_width = 0, src_height = 0;  // decoded frame size
  double fps = 0;
  size_t bytes_in = 0, bytes_out = 0;
};

/** Filter auxiliary values (cd_values, 11 of them) for these options, or an
 *  error if the codec or preset is unknown. Exposed for tests. */
bool VideoFilterValues(const VideoHdf5Options &opt, int width, int height,
                       int depth, unsigned cd_values[11], std::string *error);

/** True if the HDF5 library can load the FFMPEG filter (HDF5_PLUGIN_PATH). */
bool VideoFilterAvailable();

/** Transcode `video_path` into a new HDF5 file at `h5_path` (overwritten). */
VideoHdf5Result VideoToHdf5(const std::string &video_path,
                            const std::string &h5_path,
                            const VideoHdf5Options &opt);

}  // namespace clio::cae::core

#endif  // CLIO_CAE_CORE_FACTORY_VIDEO_HDF5_H_
