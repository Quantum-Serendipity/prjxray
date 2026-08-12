/*
 * Copyright (C) 2017-2020  The Project X-Ray Authors.
 *
 * Use of this source code is governed by a ISC-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/ISC
 *
 * SPDX-License-Identifier: ISC
 */
#include <prjxray/xilinx/frames.h>
#include <prjxray/xilinx/xc7series/ecc.h>

namespace prjxray {
namespace xilinx {
// Only block type 0 (CLB_IO_CLK) has a frame ECC on 7-series. Word 50 of a
// BLOCK_RAM frame is block RAM CONTENT, and Vivado leaves it alone: 1,792 of
// 1,792 BLOCK_RAM frames zero in a full bitstream carrying 30 initialised block
// RAMs, 772 of 772 zero across both passes of every DFX partial measured, while
// 39 CLB frames of that same full bitstream carry a nonzero ECC -- so this is a
// block-type rule and not "this design happened to be blank".
//
// This function used to run unconditionally. On a module with initialised block
// RAM it invented an ECC in the 16 frames per module whose content makes the
// polynomial nonzero, in BOTH passes of a partial and in a full bitstream, at
// exit 0: 33 words of a 244,697-word stream, all of them inside live block RAM.
// docs/far-double-write-verdict.md sections 5.1 and 9.4.
template <>
void Frames<Series7>::updateECC(
    const typename Series7::FrameAddress& address,
    typename Frames<Series7>::FrameData& data) {
	if (address.block_type() != xc7series::BlockType::CLB_IO_CLK)
		return;
	xc7series::updateECC(data);
}

// UltraScale and UltraScale+ are left as they were. The measurement above is on
// 7-series and the two families' block RAM configuration bus is not the same
// object; guessing here would be the same mistake in the other direction.
template <>
void Frames<UltraScale>::updateECC(
    const typename UltraScale::FrameAddress& address,
    typename Frames<UltraScale>::FrameData& data) {
	xc7series::updateECC(data);
}

template <>
void Frames<UltraScalePlus>::updateECC(
    const typename UltraScalePlus::FrameAddress& address,
    typename Frames<UltraScalePlus>::FrameData& data) {
	xc7series::updateECC(data);
}

// Spartan6 doesn't have ECC
template <>
void Frames<Spartan6>::updateECC(
    const typename Spartan6::FrameAddress& address,
    typename Frames<Spartan6>::FrameData& data) {}

}  // namespace xilinx
}  // namespace prjxray
