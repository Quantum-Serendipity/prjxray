/*
 * Copyright (C) 2017-2020  The Project X-Ray Authors.
 *
 * Use of this source code is governed by a ISC-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/ISC
 *
 * SPDX-License-Identifier: ISC
 */
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <prjxray/xilinx/architectures.h>
#include <prjxray/xilinx/frames.h>
#include <prjxray/xilinx/xc7series/part.h>

using namespace prjxray::xilinx;

TEST(FramesTest, FillInMissingFrames) {
	std::vector<xc7series::FrameAddress> test_part_addresses = {
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, false, 0,
	                            0, 0),
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, false, 0,
	                            0, 1),
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, false, 0,
	                            0, 2),
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, false, 0,
	                            0, 3),
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, false, 0,
	                            0, 4)};

	xc7series::Part test_part(0x1234, test_part_addresses);

	Frames<Series7> frames;
	frames.getFrames().emplace(std::make_pair(
	    xc7series::FrameAddress(2), std::vector<uint32_t>(101, 0xCC)));
	frames.getFrames().emplace(std::make_pair(
	    xc7series::FrameAddress(3), std::vector<uint32_t>(101, 0xDD)));
	frames.getFrames().emplace(std::make_pair(
	    xc7series::FrameAddress(4), std::vector<uint32_t>(101, 0xEE)));

	ASSERT_EQ(frames.getFrames().size(), 3);
	EXPECT_EQ(frames.getFrames().at(test_part_addresses[2]),
	          std::vector<uint32_t>(101, 0xCC));
	EXPECT_EQ(frames.getFrames().at(test_part_addresses[3]),
	          std::vector<uint32_t>(101, 0xDD));
	EXPECT_EQ(frames.getFrames().at(test_part_addresses[4]),
	          std::vector<uint32_t>(101, 0xEE));

	frames.addMissingFrames(test_part);

	ASSERT_EQ(frames.getFrames().size(), 5);
	EXPECT_EQ(frames.getFrames().at(test_part_addresses[0]),
	          std::vector<uint32_t>(101, 0));
	EXPECT_EQ(frames.getFrames().at(test_part_addresses[1]),
	          std::vector<uint32_t>(101, 0));
	EXPECT_EQ(frames.getFrames().at(test_part_addresses[2]),
	          std::vector<uint32_t>(101, 0xCC));
	EXPECT_EQ(frames.getFrames().at(test_part_addresses[3]),
	          std::vector<uint32_t>(101, 0xDD));
	EXPECT_EQ(frames.getFrames().at(test_part_addresses[4]),
	          std::vector<uint32_t>(101, 0xEE));
}

// readFrames computes the frame ECC on block type 0 and ONLY on block type 0.
//
// Word 50 of a CLB_IO_CLK frame is the frame ECC; word 50 of a BLOCK_RAM frame
// is block RAM content. Vivado writes no ECC there -- 1,792 of 1,792 BLOCK_RAM
// frames zero in a full bitstream carrying 30 initialised block RAMs, 772 of
// 772 zero across both passes of every DFX partial measured. This function used
// to run updateECC unconditionally, which corrupted 16 frames of live block RAM
// per module at exit 0. docs/far-double-write-verdict.md sections 5.1 and 9.4.
TEST(FramesTest, ReadFramesComputesTheEccOnBlockTypeZeroOnly) {
	// One CLB_IO_CLK and one BLOCK_RAM frame, byte-identical payloads with
	// word 50 zero, so the ONLY thing that can distinguish the two results
	// is the block type in the address.
	const uint32_t clb = static_cast<uint32_t>(xc7series::FrameAddress(
	    xc7series::BlockType::CLB_IO_CLK, false, 0, 1, 0));
	const uint32_t bram = static_cast<uint32_t>(xc7series::FrameAddress(
	    xc7series::BlockType::BLOCK_RAM, false, 0, 1, 0));

	char path[] = "/tmp/prjxray_frames_ecc_XXXXXX";
	int fd = mkstemp(path);
	ASSERT_NE(fd, -1);
	{
		std::ofstream f(path);
		for (uint32_t a : {clb, bram}) {
			f << "0x" << std::hex << a;
			for (int w = 0; w < 101; ++w)
				f << (w ? "," : " ") << "0x"
				  << (w == 0 ? "00000001" : "00000000");
			f << "\n";
		}
	}
	close(fd);

	Frames<Series7> frames;
	ASSERT_EQ(frames.readFrames(path), 0);
	std::remove(path);
	ASSERT_EQ(frames.getFrames().size(), 2u);

	// icap_ecc(0, 1, 0) is 0x1320 -- prjxray's own unit-test vector -- and
	// the whole-frame fold at word 100 flips bit 12 for odd parity, so the
	// frame's ECC is 0x0320. Nonzero either way, which is the point.
	EXPECT_EQ(frames.getFrames().at(xc7series::FrameAddress(clb))[0x32],
	          0x0320u);
	EXPECT_EQ(frames.getFrames().at(xc7series::FrameAddress(bram))[0x32],
	          0x0u);
}

TEST(FramesTest, FindFramesNotInPart) {
	std::vector<xc7series::FrameAddress> test_part_addresses = {
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, false, 0,
	                            0, 0),
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, false, 0,
	                            0, 1),
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, false, 0,
	                            0, 2)};

	xc7series::Part test_part(0x1234, test_part_addresses);

	// Minor 3 is past the end of column 0, and the part has no row 1.
	xc7series::FrameAddress past_column_end(
	    xc7series::BlockType::CLB_IO_CLK, false, 0, 0, 3);
	xc7series::FrameAddress missing_row(xc7series::BlockType::CLB_IO_CLK,
	                                    false, 1, 0, 0);

	Frames<Series7> frames;
	frames.getFrames().emplace(std::make_pair(
	    test_part_addresses[1], std::vector<uint32_t>(101, 0xCC)));
	frames.getFrames().emplace(
	    std::make_pair(past_column_end, std::vector<uint32_t>(101, 0xDD)));
	frames.getFrames().emplace(
	    std::make_pair(missing_row, std::vector<uint32_t>(101, 0xEE)));

	auto not_in_part = frames.findFramesNotInPart(test_part);

	ASSERT_EQ(not_in_part.size(), 2);
	EXPECT_EQ(static_cast<uint32_t>(not_in_part[0]),
	          static_cast<uint32_t>(past_column_end));
	EXPECT_EQ(static_cast<uint32_t>(not_in_part[1]),
	          static_cast<uint32_t>(missing_row));

	// Frames that are all in the part give an empty list.
	frames.getFrames().erase(past_column_end);
	frames.getFrames().erase(missing_row);
	EXPECT_TRUE(frames.findFramesNotInPart(test_part).empty());
}

namespace {

std::string FrameLine(uint32_t address, int words, uint32_t fill) {
	std::ostringstream line;
	line << "0x" << std::hex << address;
	for (int w = 0; w < words; ++w)
		line << (w ? "," : " ") << "0x" << fill;
	return line.str();
}

int ReadFrameLines(const std::vector<std::string>& lines,
                   Frames<Series7>* frames) {
	char path[] = "/tmp/prjxray_frames_read_XXXXXX";
	int fd = mkstemp(path);
	EXPECT_NE(fd, -1);
	{
		std::ofstream f(path);
		for (const std::string& line : lines)
			f << line << "\n";
	}
	close(fd);
	int rc = frames->readFrames(path);
	std::remove(path);
	return rc;
}

}  // namespace

TEST(FramesTest, ReadFramesAcceptsWellFormedLines) {
	Frames<Series7> frames;
	EXPECT_EQ(ReadFrameLines({"# comment", FrameLine(0x100, 101, 0x11),
	                          FrameLine(0x101, 101, 0x22)},
	                         &frames),
	          0);
	EXPECT_EQ(frames.getFrames().size(), 2u);
}

TEST(FramesTest, ReadFramesRefusesAWrongWordCount) {
	Frames<Series7> frames;
	EXPECT_NE(ReadFrameLines(
	              {FrameLine(0x100, 101, 0x11), FrameLine(0x101, 100, 0x22),
	               FrameLine(0x102, 101, 0x33)},
	              &frames),
	          0);
}

TEST(FramesTest, ReadFramesRefusesADuplicateAddress) {
	Frames<Series7> frames;
	EXPECT_NE(ReadFrameLines(
	              {FrameLine(0x100, 101, 0x11), FrameLine(0x101, 101, 0x22),
	               FrameLine(0x101, 101, 0x99)},
	              &frames),
	          0);
}

TEST(FramesTest, ReadFramesRefusesAnUnparsableLine) {
	Frames<Series7> frames;
	int rc = -1;
	EXPECT_NO_THROW(
	    rc = ReadFrameLines({FrameLine(0x100, 101, 0x11), ""}, &frames));
	EXPECT_NE(rc, 0);
	Frames<Series7> words;
	rc = -1;
	EXPECT_NO_THROW(rc = ReadFrameLines(
	                    {"0x100 0x1,zz," + std::string("0x0")}, &words));
	EXPECT_NE(rc, 0);
}
