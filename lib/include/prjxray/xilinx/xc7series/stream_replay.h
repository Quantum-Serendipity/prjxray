/*
 * Copyright (C) 2017-2020  The Project X-Ray Authors.
 *
 * Use of this source code is governed by a ISC-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/ISC
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef PRJXRAY_LIB_XILINX_XC7SERIES_STREAM_REPLAY_H_
#define PRJXRAY_LIB_XILINX_XC7SERIES_STREAM_REPLAY_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <prjxray/xilinx/xc7series/part.h>

namespace prjxray {
namespace xilinx {
namespace xc7series {

struct ReplayedFrame {
	uint32_t address;
	std::vector<uint32_t> words;
};

struct StreamReplay {
	std::vector<ReplayedFrame> committed;
	std::vector<ReplayedFrame> discarded;
	size_t dummy_frames = 0;
	size_t mfw_replays = 0;
	size_t far_writes = 0;
	bool desynced = false;
};

bool ReplayConfigurationStream(const std::vector<uint8_t>& bytes,
                               const Part& part,
                               StreamReplay* out,
                               std::string* error);

using ReplayFrameMap = std::map<FrameAddress, std::vector<uint32_t>>;

bool CheckReplayedFrames(const StreamReplay& replay,
                         const ReplayFrameMap& want,
                         const ReplayFrameMap& flush_source,
                         const ReplayFrameMap* prepass,
                         std::string* error);

}  // namespace xc7series
}  // namespace xilinx
}  // namespace prjxray

#endif  // PRJXRAY_LIB_XILINX_XC7SERIES_STREAM_REPLAY_H_
