/*
 * Copyright (C) 2017-2020  The Project X-Ray Authors.
 *
 * Use of this source code is governed by a ISC-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/ISC
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef PRJXRAY_LIB_XILINX_FRAMES_H
#define PRJXRAY_LIB_XILINX_FRAMES_H

#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <absl/strings/str_split.h>
#include <prjxray/xilinx/architectures.h>

namespace prjxray {
namespace xilinx {

// Contains frame information which is used for the generation
// of the configuration package that is used in bitstream generation.
template <typename ArchType>
class Frames {
       public:
	typedef std::vector<uint32_t> FrameData;
	typedef std::map<typename ArchType::FrameAddress, FrameData>
	    Frames2Data;

	// Reads the contents of the frames file and populates
	// the Frames container.
	int readFrames(const std::string& frm_file_str);

	// Adds empty frames that are present in the tilegrid of a specific part
	// but are missing in the current frames container.
	void addMissingFrames(
	    const absl::optional<typename ArchType::Part>& part);

	// NOTE: a second addMissingFrames() taking (clb_start, clb_end,
	// bram_start, bram_end) used to live here. It zero-filled every device
	// frame whose NUMERIC address fell inside those two intervals. An ROI
	// is a rectangle in (row, column) and an address range is an interval,
	// and the two are not the same set: on xc7a100tcsg324-1, for the
	// 1,004-frame ROI GRID_X 74..101 / GRID_Y 1..103, the interval also
	// contains row 0 columns 41..57 and row 1 columns 0..29 -- 2,032 frames
	// of live static logic, 2,031 of which were written back as zeros,
	// 801.3 KiB, at exit 0 with no warning. Its loop also hung forever at
	// 100% CPU on four inputs, including the two any user tries first (no
	// address flags at all, and a CLB-only ROI).
	//
	// The partial-bitstream writer now derives its frame set from the input
	// .frm instead; there is nothing left to fill in. See
	// Configuration::createPartialPacketData.

	// Returns the map with frame addresses and corresponding data
	Frames2Data& getFrames() { return frames_data_; }

       private:
	Frames2Data frames_data_;

	// Updates the ECC information in the frame -- IF the frame has one.
	//
	// It takes the address because on 7-series that is what decides. Word
	// 50 of a CLB_IO_CLK frame is the frame ECC; word 50 of a BLOCK_RAM
	// frame is block RAM content, and Vivado writes no ECC there at all:
	// zero in 1,792 of 1,792 BLOCK_RAM frames of a full bitstream carrying
	// 30 initialised block RAMs, and zero in 772 of 772 across both passes
	// of every partial of both DFX benches, while 39 of the same
	// bitstream's CLB frames carry a nonzero one. Computing it anyway
	// corrupts exactly the frames that hold data -- 16 per module in the
	// measured corpus -- and no frame-set checker sees it, because the
	// writer and the checker agree with each other and not with Vivado.
	// docs/far-double-write-verdict.md sections 5.1 and 9.4.
	void updateECC(const typename ArchType::FrameAddress& address,
	               FrameData& data);
};

template <typename ArchType>
int Frames<ArchType>::readFrames(const std::string& frm_file_str) {
	assert(!frm_file_str.empty());

	std::ifstream frm_file(frm_file_str);
	if (!frm_file) {
		std::cerr << "Unable to open frm file: " << frm_file_str
		          << std::endl;
		return 1;
	}
	std::string frm_line;

	while (std::getline(frm_file, frm_line)) {
		if (frm_line[0] == '#')
			continue;

		std::pair<std::string, std::string> frame_delta =
		    absl::StrSplit(frm_line, ' ');

		uint32_t frame_address =
		    std::stoul(frame_delta.first, nullptr, 16);

		std::vector<std::string> frame_data_strings =
		    absl::StrSplit(frame_delta.second, ',');

		// Spartan6's IOB frames can have different word count
		if (!std::is_same<ArchType, Spartan6>::value) {
			if (frame_data_strings.size() !=
			    ArchType::words_per_frame) {
				std::cerr
				    << "Frame " << std::hex << frame_address
				    << ": found " << std::dec
				    << frame_data_strings.size()
				    << " words instead of "
				    << ArchType::words_per_frame << std::endl;
				continue;
			}
		}

		FrameData frame_data(frame_data_strings.size(), 0);
		std::transform(frame_data_strings.begin(),
		               frame_data_strings.end(), frame_data.begin(),
		               [](const std::string& val) -> uint32_t {
			               return std::stoul(val, nullptr, 16);
		               });

		typename ArchType::FrameAddress frm_addr(frame_address);
		updateECC(frm_addr, frame_data);

		// Insert the frame address and corresponding frame data to the
		// map
		frames_data_.insert(
		    std::pair<typename ArchType::FrameAddress, FrameData>(
		        frm_addr, frame_data));
	}
	return 0;
}

template <typename ArchType>
void Frames<ArchType>::addMissingFrames(
    const absl::optional<typename ArchType::Part>& part) {
	auto current_frame_address =
	    absl::optional<typename ArchType::FrameAddress>(
	        typename ArchType::FrameAddress(0));
	do {
		auto iter = frames_data_.find(*current_frame_address);
		if (iter == frames_data_.end()) {
			FrameData frame_data(ArchType::words_per_frame, 0);
			frames_data_.insert(
			    std::pair<typename ArchType::FrameAddress,
			              FrameData>(*current_frame_address,
			                         frame_data));
		}
		current_frame_address =
		    part->GetNextFrameAddress(*current_frame_address);
	} while (current_frame_address);
}

}  // namespace xilinx
}  // namespace prjxray

#endif  // PRJXRAY_LIB_XILINX_FRAMES_H
