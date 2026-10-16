/*
 * Copyright (C) 2017-2020  The Project X-Ray Authors.
 *
 * Use of this source code is governed by a ISC-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/ISC
 *
 * SPDX-License-Identifier: ISC
 */
#include <cstdint>
#include <deque>
#include <iostream>
#include <vector>

#include <absl/types/span.h>
#include <gtest/gtest.h>
#include <prjxray/memory_mapped_file.h>
#include <prjxray/xilinx/architectures.h>
#include <prjxray/xilinx/bitstream_reader.h>
#include <prjxray/xilinx/configuration.h>
#include <prjxray/xilinx/configuration_packet.h>
#include <prjxray/xilinx/configuration_packet_with_payload.h>
#include <prjxray/xilinx/configuration_register.h>
#include <prjxray/xilinx/frames.h>
#include <prjxray/xilinx/nop_packet.h>
#include <prjxray/xilinx/xc7series/command.h>
#include <prjxray/xilinx/xc7series/configuration_options_1_value.h>
#include <prjxray/xilinx/xc7series/ecc.h>
#include <prjxray/xilinx/xc7series/prepass_geometry.h>
#include <yaml-cpp/yaml.h>

using namespace prjxray::xilinx;

TEST(ConfigurationTest, ConstructFromPacketsWithSingleFrame) {
	std::vector<Series7::FrameAddress> test_part_addresses;
	test_part_addresses.push_back(0x4567);
	test_part_addresses.push_back(0x4568);

	xc7series::Part test_part(0x1234, test_part_addresses);

	std::vector<uint32_t> idcode{0x1234};
	std::vector<uint32_t> cmd{0x0001};
	std::vector<uint32_t> frame_address{0x4567};
	std::vector<uint32_t> frame(101, 0xAA);

	std::vector<ConfigurationPacket<Series7::ConfRegType>> packets{
	    {
	        static_cast<unsigned int>(0x1),
	        ConfigurationPacket<Series7::ConfRegType>::Opcode::Write,
	        Series7::ConfRegType::IDCODE,
	        absl::MakeSpan(idcode),
	    },
	    {
	        static_cast<unsigned int>(0x1),
	        ConfigurationPacket<Series7::ConfRegType>::Opcode::Write,
	        Series7::ConfRegType::FAR,
	        absl::MakeSpan(frame_address),
	    },
	    {
	        static_cast<unsigned int>(0x1),
	        ConfigurationPacket<Series7::ConfRegType>::Opcode::Write,
	        Series7::ConfRegType::CMD,
	        absl::MakeSpan(cmd),
	    },
	    {
	        static_cast<unsigned int>(0x1),
	        ConfigurationPacket<Series7::ConfRegType>::Opcode::Write,
	        Series7::ConfRegType::FDRI,
	        absl::MakeSpan(frame),
	    },
	};

	auto test_config =
	    Configuration<Series7>::InitWithPackets(test_part, packets);
	ASSERT_TRUE(test_config);

	EXPECT_EQ(test_config->part().idcode(), static_cast<uint32_t>(0x1234));
	EXPECT_EQ(test_config->frames().size(), static_cast<size_t>(1));
	EXPECT_EQ(test_config->frames().at(0x4567), frame);
}

TEST(ConfigurationTest, ConstructFromPacketsWithAutoincrement) {
	std::vector<xc7series::FrameAddress> test_part_addresses;
	for (int ii = 0x4560; ii < 0x4570; ++ii) {
		test_part_addresses.push_back(ii);
	}
	for (int ii = 0x4580; ii < 0x4590; ++ii) {
		test_part_addresses.push_back(ii);
	}

	xc7series::Part test_part(0x1234, test_part_addresses);

	std::vector<uint32_t> idcode{0x1234};
	std::vector<uint32_t> cmd{0x0001};
	std::vector<uint32_t> frame_address{0x456F};
	std::vector<uint32_t> frame(202, 0xAA);
	std::fill_n(frame.begin() + 101, 101, 0xBB);

	std::vector<ConfigurationPacket<Series7::ConfRegType>> packets{
	    {
	        static_cast<unsigned int>(0x1),
	        ConfigurationPacket<Series7::ConfRegType>::Opcode::Write,
	        Series7::ConfRegType::IDCODE,
	        absl::MakeSpan(idcode),
	    },
	    {
	        static_cast<unsigned int>(0x1),
	        ConfigurationPacket<Series7::ConfRegType>::Opcode::Write,
	        Series7::ConfRegType::FAR,
	        absl::MakeSpan(frame_address),
	    },
	    {
	        static_cast<unsigned int>(0x1),
	        ConfigurationPacket<Series7::ConfRegType>::Opcode::Write,
	        Series7::ConfRegType::CMD,
	        absl::MakeSpan(cmd),
	    },
	    {
	        static_cast<unsigned int>(0x1),
	        ConfigurationPacket<Series7::ConfRegType>::Opcode::Write,
	        Series7::ConfRegType::FDRI,
	        absl::MakeSpan(frame),
	    },
	};

	auto test_config =
	    Configuration<Series7>::InitWithPackets(test_part, packets);
	ASSERT_TRUE(test_config);

	absl::Span<uint32_t> frame_span(frame);
	EXPECT_EQ(test_config->part().idcode(), static_cast<uint32_t>(0x1234));
	EXPECT_EQ(test_config->frames().size(), static_cast<size_t>(2));
	EXPECT_EQ(test_config->frames().at(0x456F),
	          std::vector<uint32_t>(101, 0xAA));
	EXPECT_EQ(test_config->frames().at(0x4580),
	          std::vector<uint32_t>(101, 0xBB));
}

TEST(ConfigurationTest,
     DebugAndPerFrameCrcBitstreamsProduceEqualConfigurations) {
	auto part = xc7series::Part::FromFile("configuration_test.yaml");
	ASSERT_TRUE(part);

	auto debug_bitstream = prjxray::MemoryMappedFile::InitWithFile(
	    "configuration_test.debug.bit");
	ASSERT_TRUE(debug_bitstream);

	auto debug_reader = BitstreamReader<Series7>::InitWithBytes(
	    debug_bitstream->as_bytes());
	ASSERT_TRUE(debug_reader);

	auto debug_configuration =
	    Configuration<Series7>::InitWithPackets(*part, *debug_reader);
	ASSERT_TRUE(debug_configuration);

	auto perframecrc_bitstream = prjxray::MemoryMappedFile::InitWithFile(
	    "configuration_test.perframecrc.bit");
	ASSERT_TRUE(perframecrc_bitstream);

	auto perframecrc_reader = BitstreamReader<Series7>::InitWithBytes(
	    perframecrc_bitstream->as_bytes());
	ASSERT_TRUE(perframecrc_reader);

	auto perframecrc_configuration =
	    Configuration<Series7>::InitWithPackets(*part, *perframecrc_reader);
	ASSERT_TRUE(perframecrc_configuration);

	for (auto debug_frame : debug_configuration->frames()) {
		auto perframecrc_frame =
		    perframecrc_configuration->frames().find(debug_frame.first);
		if (perframecrc_frame ==
		    perframecrc_configuration->frames().end()) {
			ADD_FAILURE() << debug_frame.first
			              << ": missing in perframecrc bitstream";
			continue;
		}

		for (int ii = 0; ii < 101; ++ii) {
			EXPECT_EQ(perframecrc_frame->second[ii],
			          debug_frame.second[ii])
			    << debug_frame.first << ": word " << ii;
		}
	}

	for (auto perframecrc_frame : perframecrc_configuration->frames()) {
		auto debug_frame =
		    debug_configuration->frames().find(perframecrc_frame.first);
		if (debug_frame == debug_configuration->frames().end()) {
			ADD_FAILURE() << perframecrc_frame.first
			              << ": unexpectedly present in "
			                 "perframecrc bitstream";
		}
	}
}

TEST(ConfigurationTest, DebugAndNormalBitstreamsProduceEqualConfigurations) {
	auto part = xc7series::Part::FromFile("configuration_test.yaml");
	ASSERT_TRUE(part);

	auto debug_bitstream = prjxray::MemoryMappedFile::InitWithFile(
	    "configuration_test.debug.bit");
	ASSERT_TRUE(debug_bitstream);

	auto debug_reader = BitstreamReader<Series7>::InitWithBytes(
	    debug_bitstream->as_bytes());
	ASSERT_TRUE(debug_reader);

	auto debug_configuration =
	    Configuration<Series7>::InitWithPackets(*part, *debug_reader);
	ASSERT_TRUE(debug_configuration);

	auto normal_bitstream =
	    prjxray::MemoryMappedFile::InitWithFile("configuration_test.bit");
	ASSERT_TRUE(normal_bitstream);

	auto normal_reader = BitstreamReader<Series7>::InitWithBytes(
	    normal_bitstream->as_bytes());
	ASSERT_TRUE(normal_reader);

	auto normal_configuration =
	    Configuration<Series7>::InitWithPackets(*part, *normal_reader);
	ASSERT_TRUE(normal_configuration);

	for (auto debug_frame : debug_configuration->frames()) {
		auto normal_frame =
		    normal_configuration->frames().find(debug_frame.first);
		if (normal_frame == normal_configuration->frames().end()) {
			ADD_FAILURE() << debug_frame.first
			              << ": missing in normal bitstream";
			continue;
		}

		for (int ii = 0; ii < 101; ++ii) {
			EXPECT_EQ(normal_frame->second[ii],
			          debug_frame.second[ii])
			    << debug_frame.first << ": word " << ii;
		}
	}

	for (auto normal_frame : normal_configuration->frames()) {
		auto debug_frame =
		    debug_configuration->frames().find(normal_frame.first);
		if (debug_frame == debug_configuration->frames().end()) {
			ADD_FAILURE()
			    << normal_frame.first
			    << ": unexpectedly present in normal bitstream";
		}
	}
}

TEST(ConfigurationTest, CheckForPaddingFrames) {
	std::vector<xc7series::FrameAddress> test_part_addresses = {
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, false, 0,
	                            0, 0),
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, true, 0,
	                            0, 0),
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, true, 1,
	                            0, 0),
	    xc7series::FrameAddress(xc7series::BlockType::BLOCK_RAM, false, 0,
	                            0, 0),
	    xc7series::FrameAddress(xc7series::BlockType::BLOCK_RAM, false, 1,
	                            0, 0)};

	auto test_part = absl::optional<xc7series::Part>(
	    xc7series::Part(0x1234, test_part_addresses));

	Frames<Series7> frames;
	frames.getFrames().emplace(std::make_pair(
	    test_part_addresses.at(0), std::vector<uint32_t>(101, 0xAA)));
	frames.getFrames().emplace(std::make_pair(
	    test_part_addresses.at(1), std::vector<uint32_t>(101, 0xBB)));
	frames.getFrames().emplace(std::make_pair(
	    test_part_addresses.at(2), std::vector<uint32_t>(101, 0xCC)));
	frames.getFrames().emplace(std::make_pair(
	    test_part_addresses.at(3), std::vector<uint32_t>(101, 0xDD)));
	frames.getFrames().emplace(std::make_pair(
	    test_part_addresses.at(4), std::vector<uint32_t>(101, 0xEE)));
	ASSERT_EQ(frames.getFrames().size(), 5);

	Configuration<Series7>::PacketData packet_data =
	    Configuration<Series7>::createType2ConfigurationPacketData(
	        frames.getFrames(), test_part);
	// createType2ConfigurationPacketData should detect 4
	// row/half/block_type switches thus add 4*2 padding frames  moreover 2
	// extra padding frames are added at the end of the creation of the data
	// overall this gives us: 5(real frames) + 4*2 + 2 = 15 frames, which is
	// 15 * 101 = 1515 words
	EXPECT_EQ(packet_data.size(), 15 * 101);

	std::vector<uint32_t> idcode{0x1234};
	std::vector<uint32_t> cmd{0x0001};
	std::vector<uint32_t> frame_address{0x0};

	std::vector<ConfigurationPacket<Series7::ConfRegType>> packets{
	    {
	        static_cast<unsigned int>(0x1),
	        ConfigurationPacket<Series7::ConfRegType>::Opcode::Write,
	        Series7::ConfRegType::IDCODE,
	        absl::MakeSpan(idcode),
	    },
	    {
	        static_cast<unsigned int>(0x1),
	        ConfigurationPacket<Series7::ConfRegType>::Opcode::Write,
	        Series7::ConfRegType::FAR,
	        absl::MakeSpan(frame_address),
	    },
	    {
	        static_cast<unsigned int>(0x1),
	        ConfigurationPacket<Series7::ConfRegType>::Opcode::Write,
	        Series7::ConfRegType::CMD,
	        absl::MakeSpan(cmd),
	    },
	    {
	        static_cast<unsigned int>(0x1),
	        ConfigurationPacket<Series7::ConfRegType>::Opcode::Write,
	        Series7::ConfRegType::FDRI,
	        absl::MakeSpan(packet_data),
	    },
	};

	auto test_config =
	    Configuration<Series7>::InitWithPackets(*test_part, packets);
	ASSERT_EQ(test_config->frames().size(), 5);
	for (auto& frame : test_config->frames()) {
		EXPECT_EQ(frame.second, frames.getFrames().at(frame.first));
	}
}

TEST(ConfigurationTest, CheckLastFrameOverwrite) {
	auto part = xc7series::Part::FromFile("configuration_test.yaml");
	ASSERT_TRUE(part);
	auto normal_bitstream = prjxray::MemoryMappedFile::InitWithFile(
	    "configuration_test_type_1.bit");
	ASSERT_TRUE(normal_bitstream);

	auto normal_reader = BitstreamReader<Series7>::InitWithBytes(
	    normal_bitstream->as_bytes());
	ASSERT_TRUE(normal_reader);

	auto normal_configuration =
	    Configuration<Series7>::InitWithPackets(*part, *normal_reader);
	ASSERT_TRUE(normal_configuration);

	auto& frames = normal_configuration->frames();
	auto& last_frame = *frames.rbegin();
	EXPECT_EQ(last_frame.second.at(4), 0x8000);
}

// ---------------------------------------------------------------------------
// Partial bitstreams.
//
// A part with two CLB_IO_CLK columns of 4 frames each in row 0 and one in
// row 1, plus two BLOCK_RAM columns. Small enough to write the expected frame
// set out by hand, and big enough to have the three discontinuities that
// matter: a column that the caller only half-covers, a row boundary, and a
// block-type boundary.
namespace {

xc7series::Part MakeTestPart() {
	std::vector<xc7series::FrameAddress> addresses;
	auto add = [&addresses](xc7series::BlockType blk, int row, int col,
	                        int frames) {
		for (int m = 0; m < frames; ++m)
			addresses.push_back(xc7series::FrameAddress(
			    blk, false, row, col, m));
	};
	add(xc7series::BlockType::CLB_IO_CLK, 0, 0, 4);
	add(xc7series::BlockType::CLB_IO_CLK, 0, 1, 4);
	add(xc7series::BlockType::CLB_IO_CLK, 1, 0, 4);
	add(xc7series::BlockType::BLOCK_RAM, 0, 0, 4);
	add(xc7series::BlockType::BLOCK_RAM, 0, 1, 4);
	return xc7series::Part(0x1234, addresses);
}

uint32_t Addr(xc7series::BlockType blk, int row, int col, int minor) {
	return static_cast<uint32_t>(
	    xc7series::FrameAddress(blk, false, row, col, minor));
}

void AddFrame(Frames<Series7>::Frames2Data* frames,
              uint32_t address,
              uint32_t fill) {
	frames->emplace(xc7series::FrameAddress(address),
	                std::vector<uint32_t>(101, fill));
}

// A stand-in for the LOCKED STATIC's frames over the whole test part, which is
// what --static_frames carries and what every flush frame is taken from. The
// fill is deliberately NOT zero: an all-zero static would make "the writer
// emitted the static's content" and "the writer emitted zeros" the same bytes,
// and the second is the payload that erases live logic if the device ever
// commits a flush frame.
Frames<Series7>::Frames2Data StaticOverTestPart(uint32_t fill = 0x5A5A5A5Au) {
	Frames<Series7>::Frames2Data out;
	auto part = MakeTestPart();
	absl::optional<xc7series::FrameAddress> a(
	    xc7series::FrameAddress(Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0,
	                                 0)));
	while (a) {
		AddFrame(&out, static_cast<uint32_t>(*a), fill);
		a = part.GetNextFrameAddress(*a);
	}
	return out;
}

}  // namespace

// Derive-from-input: the emitted frame set is the input frame set, and a gap
// in the input becomes a new FAR block rather than a run of zero frames.
TEST(ConfigurationTest, PartialPacketDataDerivesFromInput) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	auto statics = StaticOverTestPart();
	Frames<Series7>::Frames2Data frames;
	// Two frames at the end of CLB row 0 col 0, then a GAP over col 1
	// minors 0-1, then col 1 minors 2-3.
	AddFrame(&frames, Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, 2), 0xAA);
	AddFrame(&frames, Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, 3), 0xBB);
	AddFrame(&frames, Addr(xc7series::BlockType::CLB_IO_CLK, 0, 1, 2), 0xCC);
	AddFrame(&frames, Addr(xc7series::BlockType::CLB_IO_CLK, 0, 1, 3), 0xDD);
	// A whole BRAM column, which is also a block-type boundary.
	for (int m = 0; m < 4; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::BLOCK_RAM, 0, 0, m), 0xEE);

	auto blocks = Configuration<Series7>::createPartialPacketData(
	    frames, statics, part, /*pad_frames=*/0);

	// Three runs: [clb r0 c0 m2..3], [clb r0 c1 m2..3], [bram r0 c0 m0..3].
	//
	// TWO OF THE THREE CARRY A FLUSH FRAME, and which two is decided by
	// where each block stops, not by how long it is. The device commits
	// N-1 of an FDRI burst's N frames, so a block whose last frame is not
	// at a row-group end needs one frame past it:
	//   blocks[0]  clb r0 c0 m2..3 -> next is clb r0 c1 m0, SAME row
	//              group, no dummy position to fall into  -> FLUSH
	//   blocks[1]  clb r0 c1 m2..3 -> next is clb ROW 1, a row-group end,
	//              where the counter's two dummy positions are and where
	//              --pad_frames applies  -> NO flush
	//   blocks[2]  bram r0 c0 m0..3 -> next is bram r0 c1 m0, same row
	//              group  -> FLUSH
	ASSERT_EQ(blocks.size(), 3u);
	EXPECT_EQ(blocks[0].first,
	          Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, 2));
	EXPECT_EQ(blocks[0].second.size(), 3u * 101);
	EXPECT_EQ(blocks[1].first,
	          Addr(xc7series::BlockType::CLB_IO_CLK, 0, 1, 2));
	EXPECT_EQ(blocks[1].second.size(), 2u * 101);
	EXPECT_EQ(blocks[2].first,
	          Addr(xc7series::BlockType::BLOCK_RAM, 0, 0, 0));
	EXPECT_EQ(blocks[2].second.size(), 5u * 101);

	// Total words == the input's frames plus the TWO flush frames. Nothing
	// else was invented.
	size_t words = 0;
	for (auto& b : blocks)
		words += b.second.size();
	EXPECT_EQ(words, (frames.size() + 2) * 101);

	// And the payloads are the caller's, in order.
	EXPECT_EQ(blocks[0].second.front(), 0xAAu);
	EXPECT_EQ(blocks[0].second.at(101), 0xBBu);
	EXPECT_EQ(blocks[1].second.front(), 0xCCu);

	// The frame-set check agrees, walking forward from each FAR.
	EXPECT_EQ(Configuration<Series7>::checkPartialFrameSet(blocks, frames,
	                                                       statics, part, 0),
	          frames.size());
}

// A run that crosses a row boundary is split, because the hardware inserts two
// pad frames there and a block spanning one would need padding in the middle.
TEST(ConfigurationTest, PartialPacketDataSplitsAtRowBoundary) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	auto statics = StaticOverTestPart();
	Frames<Series7>::Frames2Data frames;
	for (int m = 0; m < 4; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 0, 1, m), 0x11);
	for (int m = 0; m < 4; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 1, 0, m), 0x22);

	auto blocks = Configuration<Series7>::createPartialPacketData(
	    frames, statics, part, /*pad_frames=*/0);
	ASSERT_EQ(blocks.size(), 2u);
	EXPECT_EQ(blocks[0].first,
	          Addr(xc7series::BlockType::CLB_IO_CLK, 0, 1, 0));
	EXPECT_EQ(blocks[1].first,
	          Addr(xc7series::BlockType::CLB_IO_CLK, 1, 0, 0));
	EXPECT_EQ(Configuration<Series7>::checkPartialFrameSet(blocks, frames,
	                                                       statics, part, 0),
	          8u);
}

// --pad_frames defaults to 0. When it is raised, pads go ONLY on a block that
// ends at a row/bus boundary -- which is where the device has dummy frame
// positions and where Vivado puts them. A block that stops mid-row gets none,
// because there a pad would land on a live static frame.
TEST(ConfigurationTest, PartialPacketDataPadsOnlyAtRowEnds) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	auto statics = StaticOverTestPart();

	// Block ending MID-ROW: col 0 minors 0-2 of a 4-frame column.
	Frames<Series7>::Frames2Data midrow;
	for (int m = 0; m < 3; ++m)
		AddFrame(&midrow,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, m), 0x33);
	auto a = Configuration<Series7>::createPartialPacketData(midrow, statics,
	                                                        part, 0);
	auto b = Configuration<Series7>::createPartialPacketData(midrow, statics,
	                                                        part, 2);
	ASSERT_EQ(a.size(), 1u);
	ASSERT_EQ(b.size(), 1u);
	// 3 real + 1 FLUSH, and --pad_frames does not move it: pads are the
	// row-end object and this block does not reach a row end. The flush
	// frame is not a pad and carries the static, not zeros.
	EXPECT_EQ(a[0].second.size(), 4u * 101);
	EXPECT_EQ(b[0].second.size(), 4u * 101);
	for (size_t i = 3 * 101; i < a[0].second.size(); ++i)
		EXPECT_EQ(a[0].second[i], 0x5A5A5A5Au);

	// Block ending at a ROW END: all of CLB row 0, cols 0 and 1.
	Frames<Series7>::Frames2Data rowend;
	for (int col = 0; col < 2; ++col)
		for (int m = 0; m < 4; ++m)
			AddFrame(&rowend,
			         Addr(xc7series::BlockType::CLB_IO_CLK, 0, col,
			              m),
			         0x44);
	auto none = Configuration<Series7>::createPartialPacketData(rowend, statics,
	                                                            part, 0);
	auto padded = Configuration<Series7>::createPartialPacketData(rowend, statics,
	                                                              part, 2);
	ASSERT_EQ(none.size(), 1u);
	ASSERT_EQ(padded.size(), 1u);
	EXPECT_EQ(none[0].second.size(), 8u * 101);
	EXPECT_EQ(padded[0].second.size(), 10u * 101);
	for (size_t i = 8 * 101; i < padded[0].second.size(); ++i)
		EXPECT_EQ(padded[0].second[i], 0u);

	// Either way the frame SET is the input's, and the check knows which
	// padding it was built with.
	EXPECT_EQ(Configuration<Series7>::checkPartialFrameSet(none, rowend,
	                                                       statics, part, 0),
	          8u);
	EXPECT_EQ(Configuration<Series7>::checkPartialFrameSet(padded, rowend,
	                                                       statics, part, 2),
	          8u);
	EXPECT_THROW(Configuration<Series7>::checkPartialFrameSet(padded, rowend,
	                                                          statics, part, 0),
	             std::runtime_error);
}

// The check must discriminate, not merely pass: a block whose length claims
// more frames than the caller asked for is rejected, naming the first address
// outside the input.
TEST(ConfigurationTest, PartialFrameSetCheckRejectsALeak) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	auto statics = StaticOverTestPart();
	Frames<Series7>::Frames2Data frames;
	for (int m = 0; m < 2; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, m), 0x44);

	auto blocks = Configuration<Series7>::createPartialPacketData(frames, statics,
	                                                              part, 0);
	ASSERT_EQ(Configuration<Series7>::checkPartialFrameSet(blocks, frames,
	                                                       statics, part, 0),
	          2u);
	// The block is 2 real frames + 1 flush frame, so a LEAK is now a FOURTH
	// frame. Grow it by one without telling the caller -- exactly what the
	// zero-fill design did, 2,032 times over.
	blocks[0].second.resize(4 * 101, 0);
	EXPECT_THROW(Configuration<Series7>::checkPartialFrameSet(blocks, frames,
	                                                          statics, part, 0),
	             std::runtime_error);
	// ...and a block that drops a frame is rejected too.
	blocks[0].second.resize(1 * 101, 0);
	EXPECT_THROW(Configuration<Series7>::checkPartialFrameSet(blocks, frames,
	                                                          statics, part, 0),
	             std::runtime_error);
}

// THE FLUSH FRAME. The device commits N-1 of the N frames shifted into an FDRI
// burst and discards the pending one at the next FAR write -- measured by ICAP
// at N = 2, 3 and 4 on two independent instruments
// (docs/icap-rung4-fdri-and-the-flush-frame.md sections 8, 10 and 12) -- so a
// block ending mid-row-group must carry one frame past its last, or that last
// frame never reaches configuration memory. Measured on silicon 2026-09-03: a
// 72-frame ROI-T partial loaded twice and the readback found 71 committed both
// times, with 0x000010A3 still holding the resident static.
//
// FOUR ASSERTIONS, and the last two are the ones that make the first two mean
// something: the frame is at the right address, it carries the STATIC and not
// zeros, it is NOT counted as a written frame, and a checker fed a block
// without it -- which is every partial this tree emitted before 2026-09-03 --
// REFUSES.
TEST(ConfigurationTest, PartialPacketDataFlushesAMidRowBlock) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	auto statics = StaticOverTestPart();
	Frames<Series7>::Frames2Data frames;
	for (int m = 0; m < 3; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, m), 0x77);

	auto blocks = Configuration<Series7>::createPartialPacketData(
	    frames, statics, part, 0);
	ASSERT_EQ(blocks.size(), 1u);
	ASSERT_EQ(blocks[0].second.size(), 4u * 101);

	// 1. the address. The flush frame occupies the slot the device's
	//    frame-address counter reaches after the block's last real frame.
	const uint32_t flush_at =
	    Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, 3);
	EXPECT_EQ(static_cast<uint32_t>(
	              *part->GetNextFrameAddress(xc7series::FrameAddress(
	                  Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, 2)))),
	          flush_at);
	// 2. the content is the STATIC's at that address, all 101 words.
	const auto& want = statics.at(xc7series::FrameAddress(flush_at));
	for (size_t w = 0; w < 101; ++w)
		EXPECT_EQ(blocks[0].second[3 * 101 + w], want[w]) << "word " << w;

	// 3. it is not a written frame. checkPartialFrameSet returns the count
	//    --expected_frames is compared against, and a flush frame that
	//    counted would make every ROI's declared frame count wrong by the
	//    number of its FAR blocks.
	std::set<uint32_t> flush;
	EXPECT_EQ(Configuration<Series7>::checkPartialFrameSet(
	              blocks, frames, statics, part, 0, &flush),
	          3u);
	EXPECT_EQ(flush, std::set<uint32_t>({flush_at}));

	// 4. RED. A block that stops on its own last frame is the pre-2026-09-03
	//    stream, and the checker must refuse it rather than counting 3.
	auto unterminated = blocks;
	unterminated[0].second.resize(3 * 101);
	EXPECT_THROW(Configuration<Series7>::checkPartialFrameSet(
	                 unterminated, frames, statics, part, 0),
	             std::runtime_error);

	// ...and an all-zero flush frame is refused too, which is the whole
	// reason the content is checked at all: byteman's flush frame is zeros,
	// and zeros at a live static address erase it if the device ever
	// commits them.
	auto zeroed = blocks;
	for (size_t w = 0; w < 101; ++w)
		zeroed[0].second[3 * 101 + w] = 0;
	EXPECT_THROW(Configuration<Series7>::checkPartialFrameSet(
	                 zeroed, frames, statics, part, 0),
	             std::runtime_error);

	// ...and the WRITER refuses when the static has no frame at the flush
	// address, rather than substituting zeros itself.
	Frames<Series7>::Frames2Data no_flush_source;
	for (int m = 0; m < 3; ++m)
		AddFrame(&no_flush_source,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, m), 0x11);
	EXPECT_THROW(Configuration<Series7>::createPartialPacketData(
	                 frames, no_flush_source, part, 0),
	             std::runtime_error);
}

// A block that ends AT a row-group end is flushed by its first PAD, so it must
// NOT also get a flush frame -- that would be a third frame past a block Vivado
// terminates with two. The pad path is unchanged and this is what says so.
TEST(ConfigurationTest, PartialPacketDataDoesNotFlushARowEndBlock) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	auto statics = StaticOverTestPart();
	Frames<Series7>::Frames2Data rowend;
	for (int col = 0; col < 2; ++col)
		for (int m = 0; m < 4; ++m)
			AddFrame(&rowend,
			         Addr(xc7series::BlockType::CLB_IO_CLK, 0, col,
			              m),
			         0x88);
	auto padded = Configuration<Series7>::createPartialPacketData(
	    rowend, statics, part, 2);
	ASSERT_EQ(padded.size(), 1u);
	// 8 real + 2 pads, NOT 8 + 2 + 1.
	EXPECT_EQ(padded[0].second.size(), 10u * 101);
	for (size_t i = 8 * 101; i < padded[0].second.size(); ++i)
		EXPECT_EQ(padded[0].second[i], 0u);
	std::set<uint32_t> flush;
	EXPECT_EQ(Configuration<Series7>::checkPartialFrameSet(
	              padded, rowend, statics, part, 2, &flush),
	          8u);
	EXPECT_TRUE(flush.empty());
}

// An address the part does not have is an error, not a silent walk off the end
// of the device. This is one of the four inputs that used to spin at 100% CPU.
TEST(ConfigurationTest, PartialPacketDataRejectsInvalidAddress) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	auto statics = StaticOverTestPart();
	Frames<Series7>::Frames2Data frames;
	AddFrame(&frames, Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, 0), 0x55);
	AddFrame(&frames, Addr(xc7series::BlockType::CLB_IO_CLK, 7, 9, 0), 0x66);
	EXPECT_THROW(
	    Configuration<Series7>::createPartialPacketData(frames, statics, part, 0),
	    std::runtime_error);
}

// Antmicro's original test, adapted to the derive-from-input API: the package
// still carries one FAR write per block, in ascending address order, with the
// CLB block before the BRAM one.
TEST(ConfigurationTest, CreatePartialConfigurationPackage) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	auto statics = StaticOverTestPart();
	Frames<Series7>::Frames2Data frames;
	for (int m = 0; m < 4; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, m), 0xAB);
	for (int m = 0; m < 4; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::BLOCK_RAM, 0, 0, m), 0xBC);

	// NOTE: cfg_packets must outlive partial_configuration_package. The
	// FDRI packets hold non-owning spans over it.
	auto cfg_packets = Configuration<Series7>::createPartialPacketData(
	    frames, statics, part, 0);
	Series7::ConfigurationPackage partial_configuration_package;
	Configuration<Series7>::createPartialConfigurationPackage(
	    partial_configuration_package, cfg_packets, part,
	    PartialStreamOptions());

	std::vector<uint32_t> far_writes;
	for (auto& cfg : partial_configuration_package) {
		if (cfg->address() == Series7::ConfRegType::FAR)
			far_writes.push_back(cfg->data()[0]);
	}
	// Two data blocks plus the trailing park FAR. 0x03be0000 decodes to
	// block_type 7, reserved on every 7-series part, and Vivado emits it
	// itself -- it is deliberately kept.
	ASSERT_EQ(far_writes.size(), 3u);
	EXPECT_EQ(far_writes[0], Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, 0));
	EXPECT_EQ(far_writes[1], Addr(xc7series::BlockType::BLOCK_RAM, 0, 0, 0));
	EXPECT_EQ(far_writes[2], 0x03be0000u);

	// The FDRI payloads still point at live storage after the call.
	size_t fdri_words = 0;
	for (auto& cfg : partial_configuration_package) {
		if (cfg->address() == Series7::ConfRegType::FDRI)
			fdri_words += cfg->data().size();
	}
	// Two blocks, each ending mid-row-group, so each carries one flush
	// frame past the input's last.
	EXPECT_EQ(fdri_words, (frames.size() + 2) * 101);
}

// ---------------------------------------------------------------------------
// Unit 6.2: packet-sequence conformance with Vivado, and the FAR double-write
// pre-pass.
//
// The reference for every constant below is the decoded word-level map of
// Vivado 2025.2's own partial bitstreams for xc7a100tcsg324-1
// (gateware/vivado-dfx-golden/golden/packets/, reproducible with
// scripts/u62-packet-map.py) and docs/far-double-write-verdict.md section 5.1.
// ---------------------------------------------------------------------------

namespace {

// The command/register sequence of a package, as (register, first data word)
// with NOPs dropped -- the same reduction scripts/bit-packets.py applies, so
// the two can be compared by eye against the golden dumps.
std::vector<std::pair<Series7::ConfRegType, uint32_t>> RegSequence(
    const Series7::ConfigurationPackage& pkg) {
	std::vector<std::pair<Series7::ConfRegType, uint32_t>> out;
	for (auto& p : pkg) {
		if (p->opcode() !=
		    ConfigurationPacket<Series7::ConfRegType>::Opcode::Write)
			continue;
		out.emplace_back(p->address(),
		                 p->data().empty() ? 0u : p->data()[0]);
	}
	return out;
}

size_t CountCmd(const Series7::ConfigurationPackage& pkg, uint32_t cmd) {
	size_t n = 0;
	for (auto& p : pkg) {
		if (p->opcode() ==
		        ConfigurationPacket<Series7::ConfRegType>::Opcode::Write &&
		    p->address() == Series7::ConfRegType::CMD &&
		    p->data().size() == 1 && p->data()[0] == cmd)
			++n;
	}
	return n;
}

size_t CountNops(const Series7::ConfigurationPackage& pkg) {
	size_t n = 0;
	for (auto& p : pkg)
		if (p->opcode() ==
		    ConfigurationPacket<Series7::ConfRegType>::Opcode::NOP)
			++n;
	return n;
}

std::vector<uint32_t> FarSequence(const Series7::ConfigurationPackage& pkg) {
	std::vector<uint32_t> out;
	for (auto& p : pkg)
		if (p->opcode() ==
		        ConfigurationPacket<Series7::ConfRegType>::Opcode::Write &&
		    p->address() == Series7::ConfRegType::FAR &&
		    !p->data().empty())
			out.push_back(p->data()[0]);
	return out;
}

// A frame set in one configuration column, spanning the minors the pre-pass
// treats differently. Column 0 of lib/test_data/prepass_tilegrid.json is a CLB
// column whose tiles cover words 0-1 and 99-100 either side of the HCLK tile at
// word 50; column 1 is a DSP column and column 2 a BRAM column, both with a
// ten-word tile at words 0-9.
Frames<Series7>::Frames2Data ColumnFrames(uint32_t fill,
                                          const std::vector<int>& minors,
                                          uint16_t column = 0) {
	Frames<Series7>::Frames2Data f;
	for (int m : minors)
		f.emplace(xc7series::FrameAddress(
		              xc7series::BlockType::CLB_IO_CLK, false, 0,
		              column, m),
		          std::vector<uint32_t>(101, fill));
	return f;
}

Frames<Series7>::Frames2Data ClbFrames(uint32_t fill,
                                       const std::vector<int>& minors) {
	return ColumnFrames(fill, minors, 0);
}

// SITE is per tile, so every pre-pass test needs the tile lattice. The fixture
// is loaded once; a test that cannot load it must fail loudly rather than skip.
const xc7series::PrePassGeometry& TestGeometry() {
	static std::unique_ptr<xc7series::PrePassGeometry> geo = [] {
		std::string err;
		auto g = xc7series::PrePassGeometry::Load(
		    "prepass_tilegrid.json", "prepass_db", &err);
		EXPECT_TRUE(g) << err;
		return g;
	}();
	return *geo;
}

uint32_t ExpectedEcc(std::vector<uint32_t> frame) {
	xc7series::updateECC(frame);
	return frame[0x32];
}

}  // namespace

// SITE and PARK, per docs/far-double-write-verdict.md section 5.1, on the
// minors whose treatment differs. The RM fill is 0xFFFFFFFF and the static is
// empty, so whatever survives is exactly SITE|PARK.
TEST(ConfigurationTest, PrePassAppliesSiteAndParkMasks) {
	auto frames = ClbFrames(0xFFFFFFFF, {0, 1, 2, 17, 24, 25, 30});
	Frames<Series7>::Frames2Data no_static;
	auto pass1 = Configuration<Series7>::createPrePassFrames(
	    frames, no_static, /*have_static=*/false, TestGeometry(), nullptr);
	ASSERT_EQ(pass1.size(), frames.size());

	auto at = [&pass1](int minor) -> const std::vector<uint32_t>& {
		return pass1.at(xc7series::FrameAddress(
		    xc7series::BlockType::CLB_IO_CLK, false, 0, 0, minor));
	};
	// minor 0: the CLB owns bit index == 0 (mod 4).
	EXPECT_EQ(at(0)[0], 0x11111111u);
	// minor 1: bit index == 3 (mod 4).
	EXPECT_EQ(at(1)[0], 0x88888888u);
	// minors 2-23 are interconnect: withheld entirely.
	EXPECT_EQ(at(2)[0], 0x00000000u);
	EXPECT_EQ(at(17)[0], 0x00000000u);
	// minors 24-25: the routing-mux selector planes, parked high...
	EXPECT_EQ(at(24)[0], 0xFFFFFFFFu);
	EXPECT_EQ(at(24)[100], 0xFFFFFFFFu);
	EXPECT_EQ(at(25)[0], 0xFFFFFFFFu);
	// ...at every word of the frame, PARK being the one term that is not a
	// function of the tile lattice.
	EXPECT_EQ(at(24)[49], 0xFFFFFFFFu);
	// ...except word 50, which belongs to the HCLK tile and carries the ECC.
	EXPECT_EQ(at(24)[0x32] & 0xFFFFE000u, 0x00000000u);
	// minors 26-35: the site planes, minus *.PRECYINIT.CIN. Bit 13 of the
	// tile's first word is SLICE*_X0's CIN and is withheld 66 of 66; before
	// Unit 6.2b this word came out 0xFFFFFFFF and RM-A missed by that bit.
	EXPECT_EQ(at(30)[0], 0xFFFFFFFFu & ~(1u << 13));
	EXPECT_EQ(at(30)[1], 0xFFFFFFFFu);
	// A word inside the column that no tile of it covers keeps nothing at
	// all -- SITE is a property of the tile, not of the minor.
	EXPECT_EQ(at(30)[2], 0x00000000u);
	// word 50 is dropped at every minor (the ECC field aside).
	for (int m : {0, 1, 2, 17, 24, 25, 30})
		EXPECT_EQ(at(m)[0x32] & 0xFFFFE000u, 0x00000000u)
		    << "minor " << m;
}

// THE 2,987-BIT DEFECT, at the writer's own interface. Section 5.1's SITE is
// per tile: in a BRAM configuration column pass 1 keeps minor 27 and nothing
// else, and in a DSP column it keeps minors 26-27 minus the tile's own input
// muxes. The mask this replaces returned 0xFFFFFFFF for every minor in 26..35
// whatever the column was made of.
TEST(ConfigurationTest, PrePassSiteMaskFollowsTheColumnsTileTypes) {
	Frames<Series7>::Frames2Data no_static;
	// BRAM column (fixture column 2), minors 26 and 27.
	auto bram = ColumnFrames(0xFFFFFFFF, {0, 1, 26, 27}, 2);
	auto b1 = Configuration<Series7>::createPrePassFrames(
	    bram, no_static, false, TestGeometry(), nullptr);
	auto bat = [&b1](int minor) -> const std::vector<uint32_t>& {
		return b1.at(xc7series::FrameAddress(
		    xc7series::BlockType::CLB_IO_CLK, false, 0, 2, minor));
	};
	// minor 26 is the BRAM's address-input mux plane: withheld entirely.
	for (size_t w = 0; w < 10; ++w)
		EXPECT_EQ(bat(26)[w], 0x0u) << "BRAM minor 26 word " << w;
	// minor 27 carries every RAMB* attribute: kept entirely.
	for (size_t w = 0; w < 10; ++w)
		EXPECT_EQ(bat(27)[w], 0xFFFFFFFFu) << "BRAM minor 27 word " << w;
	// minors 0 and 1: everything present is the INT tile's, so the CLB
	// lattice must NOT appear here.
	EXPECT_EQ(bat(0)[0], 0x0u);
	EXPECT_EQ(bat(1)[0], 0x0u);

	// DSP column (fixture column 1).
	auto dsp = ColumnFrames(0xFFFFFFFF, {0, 26, 27}, 1);
	auto d1 = Configuration<Series7>::createPrePassFrames(
	    dsp, no_static, false, TestGeometry(), nullptr);
	auto dat = [&d1](int minor) -> const std::vector<uint32_t>& {
		return d1.at(xc7series::FrameAddress(
		    xc7series::BlockType::CLB_IO_CLK, false, 0, 1, minor));
	};
	EXPECT_EQ(dat(0)[0], 0x0u);
	// 26_50 and 26_63 are DSP_VCC_L/DSP_GND_L positions in the tile's
	// second word; everything else in that word is kept.
	EXPECT_EQ(dat(26)[1], 0xFFFFFFFFu & ~(1u << 18) & ~(1u << 31));
	// AREG_0's minor-26 bits are kept -- only the minor-27 bit of the code
	// is withheld, which is the one fitted element of the mask.
	EXPECT_EQ(dat(26)[3], 0xFFFFFFFFu);
	EXPECT_EQ(dat(27)[3], 0xFFFFFFFFu & ~(1u << 15));
}

// The two elements section 5.1 flags as INFERRED are counted, so a future
// mismatch can be attributed instead of guessed at.
TEST(ConfigurationTest, PrePassReportsWhenAnInferredMaskElementFires) {
	Frames<Series7>::Frames2Data no_static;
	PrePassReport rep;
	// Minor 31 bit 12 is SLICE*_X1's PRECYINIT.CIN -- never seen in a
	// bitstream, taken from prjxray by symmetry. Two CLB tiles cover words
	// 0-1 and 99-100 of the fixture's column 0, so two instances fire.
	auto frames = ColumnFrames(0xFFFFFFFF, {31}, 0);
	Configuration<Series7>::createPrePassFrames(frames, no_static, false,
	                                            TestGeometry(), &rep);
	EXPECT_EQ(rep.inferred_cin_x1_bits, 2u);
	EXPECT_EQ(rep.inferred_dsp_reg0_bits, 0u);

	// Minor 27 of a DSP column: four {A,B}REG_0 bits in the one tile.
	PrePassReport dsp_rep;
	auto dsp = ColumnFrames(0xFFFFFFFF, {27}, 1);
	Configuration<Series7>::createPrePassFrames(dsp, no_static, false,
	                                            TestGeometry(), &dsp_rep);
	EXPECT_EQ(dsp_rep.inferred_dsp_reg0_bits, 4u);
	EXPECT_EQ(dsp_rep.inferred_cin_x1_bits, 0u);

	// A minor where neither element applies fires neither counter, so the
	// counters are not just counting frames.
	PrePassReport quiet;
	auto plain = ColumnFrames(0xFFFFFFFF, {30}, 0);
	Configuration<Series7>::createPrePassFrames(plain, no_static, false,
	                                            TestGeometry(), &quiet);
	EXPECT_EQ(quiet.inferred_cin_x1_bits, 0u);
	EXPECT_EQ(quiet.inferred_dsp_reg0_bits, 0u);
}

// A column tilegrid.json has no tile in, and a column whose bit-bearing tile
// type section 5.1's table does not name, are different situations and are
// reported as different situations.
TEST(ConfigurationTest, PrePassReportsGeometryItDoesNotHave) {
	Frames<Series7>::Frames2Data no_static;
	PrePassReport rep;
	auto frames = ColumnFrames(0xFFFFFFFF, {30}, 5);  // not in the fixture
	Configuration<Series7>::createPrePassFrames(frames, no_static, false,
	                                            TestGeometry(), &rep);
	EXPECT_EQ(rep.columns_without_geometry.size(), 1u);
	EXPECT_TRUE(rep.unclassified_types_touched.empty());

	PrePassReport io_rep;
	auto io = ColumnFrames(0xFFFFFFFF, {30}, 3);  // the LIOI3 column
	Configuration<Series7>::createPrePassFrames(io, no_static, false,
	                                            TestGeometry(), &io_rep);
	EXPECT_TRUE(io_rep.columns_without_geometry.empty());
	EXPECT_EQ(io_rep.unclassified_types_touched.count("LIOI3"), 1u);

	// A plain CLB column reports neither, so both are not always on.
	PrePassReport clb_rep;
	auto clb = ColumnFrames(0xFFFFFFFF, {30}, 0);
	Configuration<Series7>::createPrePassFrames(clb, no_static, false,
	                                            TestGeometry(), &clb_rep);
	EXPECT_TRUE(clb_rep.columns_without_geometry.empty());
	EXPECT_TRUE(clb_rep.unclassified_types_touched.empty());
}

// The ECC is RECOMPUTED for pass 1's own content, not copied from pass 2 and
// not left zero. Vivado's pass 1 carries a correct ECC in 825 of 825 block-0
// frames.
TEST(ConfigurationTest, PrePassRecomputesTheFrameEcc) {
	// Minor 24 is parked to all-ones; minor 30 keeps a site payload whose
	// ECC is NOT zero. Two frames with different correct ECCs, so neither a
	// copied nor a constant ECC can pass.
	Frames<Series7>::Frames2Data frames;
	frames.emplace(
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, false, 0,
	                            0, 24),
	    std::vector<uint32_t>(101, 0xFFFFFFFF));
	frames.emplace(
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, false, 0,
	                            0, 30),
	    std::vector<uint32_t>(101, 0x00000001));
	Frames<Series7>::Frames2Data no_static;
	auto pass1 = Configuration<Series7>::createPrePassFrames(
	    frames, no_static, false, TestGeometry(), nullptr);

	for (auto& f : pass1) {
		std::vector<uint32_t> recomputed = f.second;
		recomputed[0x32] &= 0xFFFFE000u;
		EXPECT_EQ(f.second[0x32] & 0x1FFFu,
		          ExpectedEcc(recomputed) & 0x1FFFu)
		    << "minor " << static_cast<int>(f.first.minor());
	}
	const auto& m24 = pass1.at(xc7series::FrameAddress(
	    xc7series::BlockType::CLB_IO_CLK, false, 0, 0, 24));
	const auto& m30 = pass1.at(xc7series::FrameAddress(
	    xc7series::BlockType::CLB_IO_CLK, false, 0, 0, 30));
	// The parked all-ones frame's correct ECC really is 0 -- which is why
	// Vivado's word 50 looks blank there and why "it has no ECC" is the
	// wrong conclusion to draw from it.
	EXPECT_EQ(m24[0x32] & 0x1FFFu, 0x0u);
	// Four words survive SITE here (the fixture column's two CLB tiles at
	// words 0-1 and 99-100), so this literal also pins WHICH words the
	// per-tile mask kept: with the old (minor, word) mask all 100 survived
	// and the ECC came out 0x06A0.
	EXPECT_EQ(m30[0x32] & 0x1FFFu, 0x1040u);
}

// The STATIC term. It is what makes the pre-pass constructible, and it is the
// term that is silently missing if the writer is not given the static design.
TEST(ConfigurationTest, PrePassKeepsTheStaticsOwnInterconnect) {
	// RM content at minor 2 (interconnect) and minor 30 (site).
	auto pass2 = ClbFrames(0x0F0F0F0F, {2, 30});
	// Static content in the same frames, on disjoint bits.
	auto stat = ClbFrames(0xF000000F, {2, 30});

	auto with = Configuration<Series7>::createPrePassFrames(
	    pass2, stat, true, TestGeometry(), nullptr);
	auto without = Configuration<Series7>::createPrePassFrames(
	    pass2, stat, /*have_static=*/false, TestGeometry(), nullptr);

	auto at = [](const Frames<Series7>::Frames2Data& f,
	             int minor) -> const std::vector<uint32_t>& {
		return f.at(xc7series::FrameAddress(
		    xc7series::BlockType::CLB_IO_CLK, false, 0, 0, minor));
	};
	// Minor 2 is interconnect: the RM's bits are withheld and the static's
	// are kept. Withholding the static too is the degradation.
	EXPECT_EQ(at(with, 2)[0], 0xF000000Fu);
	EXPECT_EQ(at(without, 2)[0], 0x00000000u);
	// Minor 30 is a site plane: the RM's bits are kept, and the static's
	// contribution outside SITE is empty there, so both agree.
	EXPECT_EQ(at(with, 30)[0], 0x0F0F0F0Fu);
	EXPECT_EQ(at(without, 30)[0], 0x0F0F0F0Fu);
}

// The transformation applies to block type 0 and to NOTHING else. Every other
// block type is written twice verbatim -- same payload, ECC word included, no
// mask, no park, no recomputation. Read without a block-type term, section 5.1
// forces six whole BRAM frames to all-ones.
TEST(ConfigurationTest, PrePassLeavesNonClbBlockTypesAlone) {
	Frames<Series7>::Frames2Data frames;
	// A recognisable, non-uniform payload: "verbatim" and "zeroed" have to
	// be distinguishable, and an all-zero frame cannot tell them apart.
	std::vector<uint32_t> content(101, 0u);
	for (size_t w = 0; w < content.size(); ++w)
		content[w] = 0xB0000000u + static_cast<uint32_t>(w);
	content[0x32] = 0u;  // Vivado writes no frame ECC in a BLOCK_RAM block
	for (int m : {0, 1, 24, 25, 27, 30})
		frames.emplace(
		    xc7series::FrameAddress(xc7series::BlockType::BLOCK_RAM,
		                            false, 0, 0, m),
		    content);
	Frames<Series7>::Frames2Data no_static;
	PrePassReport rep;
	auto pass1 = Configuration<Series7>::createPrePassFrames(
	    frames, no_static, false, TestGeometry(), &rep);
	ASSERT_EQ(pass1.size(), frames.size());
	for (auto& f : pass1)
		EXPECT_EQ(f.second, content)
		    << "BRAM minor " << static_cast<int>(f.first.minor());
	EXPECT_EQ(rep.verbatim_frames, frames.size());
	EXPECT_EQ(rep.clb_frames, 0u);
}

// And prjxray's updateECC must not run there. Word 50 of every BLOCK_RAM frame
// is zero in both passes -- 0 nonzero of 772, content or no content -- and
// updateECC would invent one in exactly the 16 frames per module that carry
// initialised block RAM content.
TEST(ConfigurationTest, PrePassNeverComputesAnEccOnBlockRam) {
	// This is s1/F block 1 frame 0's situation: content whose correct ECC
	// is NOT zero, which Vivado nonetheless leaves at zero.
	std::vector<uint32_t> content(101, 0u);
	content[0] = 0x00000001u;
	ASSERT_NE(ExpectedEcc(content) & 0x1FFFu, 0x0u)
	    << "the fixture must be a frame updateECC would change";
	Frames<Series7>::Frames2Data frames;
	frames.emplace(
	    xc7series::FrameAddress(xc7series::BlockType::BLOCK_RAM, false, 0,
	                            1, 0),
	    content);
	Frames<Series7>::Frames2Data no_static;
	auto pass1 = Configuration<Series7>::createPrePassFrames(
	    frames, no_static, false, TestGeometry(), nullptr);
	const auto& out = pass1.begin()->second;
	EXPECT_EQ(out[0x32], 0x0u);
	EXPECT_EQ(out, content);

	// The same content in a CLB_IO_CLK frame DOES get an ECC, so the test
	// above is about the block type and not about the content.
	Frames<Series7>::Frames2Data clb;
	clb.emplace(xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK,
	                                    false, 0, 0, 30),
	            content);
	auto clb1 = Configuration<Series7>::createPrePassFrames(
	    clb, no_static, false, TestGeometry(), nullptr);
	EXPECT_NE(clb1.begin()->second[0x32] & 0x1FFFu, 0x0u);
}

// Word 50 is 0 plus the computed ECC, whatever the static holds there. Section
// 5.1's EXPRESSION says `STATIC[w] | ...`, which would carry the static's
// HCLK-tile bits and its stale ECC through at w == 50; its own TABLE says
// "0 + computed ECC", and the goldens agree with the table.
TEST(ConfigurationTest, PrePassWithholdsWord50FromTheStaticToo) {
	auto pass2 = ClbFrames(0x0, {2, 17});
	auto stat = ClbFrames(0xFFFFFFFF, {2, 17});
	auto pass1 = Configuration<Series7>::createPrePassFrames(
	    pass2, stat, true, TestGeometry(), nullptr);
	for (auto& f : pass1) {
		EXPECT_EQ(f.second[0x32] & 0xFFFFE000u, 0x0u)
		    << "minor " << static_cast<int>(f.first.minor());
		// ...while every OTHER word of these interconnect minors did
		// take the static's bits, so this is not passing because the
		// static was ignored wholesale.
		EXPECT_EQ(f.second[0], 0xFFFFFFFFu);
		EXPECT_EQ(f.second[100], 0xFFFFFFFFu);
	}
}

TEST(ConfigurationTest, PrePassRefusesAStaticWithoutTheFrame) {
	auto pass2 = ClbFrames(0x0F0F0F0F, {2, 30});
	auto partial_static = ClbFrames(0xF000000F, {2});
	EXPECT_THROW(Configuration<Series7>::createPrePassFrames(
	                 pass2, partial_static, true, TestGeometry(), nullptr),
	             std::runtime_error);
	EXPECT_NO_THROW(Configuration<Series7>::createPrePassFrames(
	    pass2, partial_static, false, TestGeometry(), nullptr));

	auto short_static = ClbFrames(0xF000000F, {2, 30});
	short_static.begin()->second.resize(100);
	EXPECT_THROW(Configuration<Series7>::createPrePassFrames(
	                 pass2, short_static, true, TestGeometry(), nullptr),
	             std::runtime_error);
}

TEST(ConfigurationTest, PrePassNeedsNoStaticForOtherBlockTypes) {
	Frames<Series7>::Frames2Data pass2, no_static;
	pass2.emplace(xc7series::FrameAddress(xc7series::BlockType::BLOCK_RAM,
	                                      false, 0, 0, 0),
	              std::vector<uint32_t>(101, 0x5));
	EXPECT_NO_THROW(Configuration<Series7>::createPrePassFrames(
	    pass2, no_static, true, TestGeometry(), nullptr));
}

// Not characterised anywhere but 7-series, so it must not be guessed.
TEST(ConfigurationTest, PrePassRefusesOnOtherArchitectures) {
	Frames<UltraScale>::Frames2Data frames, no_static;
	EXPECT_THROW(Configuration<UltraScale>::createPrePassFrames(
	                 frames, no_static, false, TestGeometry(), nullptr),
	             std::runtime_error);
}

// Unit 6.2 step 2, first half. Vivado emits CMD:WCFG once per FAR block;
// PR #1492 emitted it once for the whole stream.
TEST(ConfigurationTest, PartialPackageEmitsWcfgPerFarBlock) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	auto statics = StaticOverTestPart();
	Frames<Series7>::Frames2Data frames;
	for (int m = 0; m < 4; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, m), 0xAB);
	for (int m = 0; m < 4; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::BLOCK_RAM, 0, 0, m), 0xBC);
	auto blocks = Configuration<Series7>::createPartialPacketData(frames, statics,
	                                                              part, 0);
	ASSERT_EQ(blocks.size(), 2u);
	Series7::ConfigurationPackage pkg;
	Configuration<Series7>::createPartialConfigurationPackage(
	    pkg, blocks, part, PartialStreamOptions());
	EXPECT_EQ(CountCmd(pkg, static_cast<uint32_t>(xc7series::Command::WCFG)),
	          blocks.size());
	// 16 trailing NOPs, plus one before RCRC, two after it, and two per
	// block (after WCFG and after FAR). 16 + 1 + 2 + 2*2 = 23.
	EXPECT_EQ(CountNops(pkg), 23u);
	// And no GRESTORE or START anywhere, per the golden.
	EXPECT_EQ(CountCmd(pkg, 0xA), 0u);  // GRESTORE
	EXPECT_EQ(CountCmd(pkg, 0x5), 0u);  // START
	EXPECT_EQ(CountCmd(pkg, 0x3), 0u);  // DGHIGH/LFRM
}

// Unit 6.2 step 2, second half. The two passes are interleaved A B A B --
// pass 1 of EVERY block, then pass 2 of every block -- not A A B B. A
// frame-level comparator is definitionally blind to this difference, so it has
// to be asserted structurally.
TEST(ConfigurationTest, PartialPackageInterleavesTheTwoPasses) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	auto statics = StaticOverTestPart();
	Frames<Series7>::Frames2Data frames;
	for (int m = 0; m < 4; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, m), 0xAB);
	for (int m = 0; m < 4; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::BLOCK_RAM, 0, 0, m), 0xBC);
	Frames<Series7>::Frames2Data no_static;
	auto pass1_frames = Configuration<Series7>::createPrePassFrames(
	    frames, no_static, false, TestGeometry(), nullptr);

	auto pass2_blocks =
	    Configuration<Series7>::createPartialPacketData(frames, statics, part, 0);
	auto pass1_blocks = Configuration<Series7>::createPartialPacketData(
	    pass1_frames, statics, part, 0);
	auto all = pass1_blocks;
	all.insert(all.end(), pass2_blocks.begin(), pass2_blocks.end());

	Series7::ConfigurationPackage pkg;
	Configuration<Series7>::createPartialConfigurationPackage(
	    pkg, all, part, PartialStreamOptions());

	auto fars = FarSequence(pkg);
	const uint32_t a = Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, 0);
	const uint32_t b = Addr(xc7series::BlockType::BLOCK_RAM, 0, 0, 0);
	// A B A B, then the park FAR.
	ASSERT_EQ(fars.size(), 5u);
	EXPECT_EQ(fars[0], a);
	EXPECT_EQ(fars[1], b);
	EXPECT_EQ(fars[2], a);
	EXPECT_EQ(fars[3], b);
	EXPECT_EQ(fars[4], 0x03be0000u);
	// ...which is NOT A A B B. Stated as its own assertion so the intent
	// survives a future edit of the sequence above.
	EXPECT_FALSE(fars[0] == fars[1] && fars[2] == fars[3]);
	// One WCFG per emitted block, so four.
	EXPECT_EQ(CountCmd(pkg, static_cast<uint32_t>(xc7series::Command::WCFG)),
	          4u);
	// The two passes carry DIFFERENT payloads at the same FAR: an emitter
	// that wrote the same block twice would pass every check above.
	std::vector<absl::Span<const uint32_t>> type2;
	for (auto& p : pkg)
		if (p->header_type() == TYPE2)
			type2.push_back(p->data());
	ASSERT_EQ(type2.size(), 4u);
	EXPECT_NE(type2[0][0], type2[2][0]);
}

// Unit 6.2 step 4. A .bit over JTAG carries IDCODE; a raw ICAP stream does not.
TEST(ConfigurationTest, PartialPackageOmitsIdcodeInRawMode) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	auto statics = StaticOverTestPart();
	Frames<Series7>::Frames2Data frames;
	for (int m = 0; m < 4; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, m), 0xAB);
	auto blocks = Configuration<Series7>::createPartialPacketData(frames, statics,
	                                                              part, 0);
	auto count_idcode = [](const Series7::ConfigurationPackage& pkg) {
		size_t n = 0;
		for (auto& p : pkg)
			if (p->address() == Series7::ConfRegType::IDCODE &&
			    p->opcode() ==
			        ConfigurationPacket<
			            Series7::ConfRegType>::Opcode::Write)
				++n;
		return n;
	};

	PartialStreamOptions jtag;
	Series7::ConfigurationPackage bit_pkg;
	Configuration<Series7>::createPartialConfigurationPackage(
	    bit_pkg, blocks, part, jtag);
	EXPECT_EQ(count_idcode(bit_pkg), 1u);

	PartialStreamOptions icap;
	icap.emit_idcode = false;
	Series7::ConfigurationPackage raw_pkg;
	Configuration<Series7>::createPartialConfigurationPackage(
	    raw_pkg, blocks, part, icap);
	EXPECT_EQ(count_idcode(raw_pkg), 0u);
	// Exactly one packet fewer, and the CRC differs because the IDCODE
	// write is accumulated.
	EXPECT_EQ(raw_pkg.size() + 1, bit_pkg.size());
}

// Unit 6.2 step 5. Vivado's BITSTREAM.GENERAL.CRC=DISABLE substitutes exactly
// two 32-bit words per CRC write -- a Type-1 CRC write of the value becomes a
// Type-1 CMD write of RCRC -- and changes nothing else.
TEST(ConfigurationTest, PartialPackageCrcDisableIsATwoWordSubstitution) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	auto statics = StaticOverTestPart();
	Frames<Series7>::Frames2Data frames;
	for (int m = 0; m < 4; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, m), 0xAB);
	auto blocks = Configuration<Series7>::createPartialPacketData(frames, statics,
	                                                              part, 0);

	Series7::ConfigurationPackage with_crc, without_crc;
	PartialStreamOptions on;
	PartialStreamOptions off;
	off.compute_crc = false;
	Configuration<Series7>::createPartialConfigurationPackage(with_crc,
	                                                          blocks, part,
	                                                          on);
	Configuration<Series7>::createPartialConfigurationPackage(without_crc,
	                                                          blocks, part,
	                                                          off);

	ASSERT_EQ(with_crc.size(), without_crc.size());
	auto a = RegSequence(with_crc), b = RegSequence(without_crc);
	ASSERT_EQ(a.size(), b.size());
	size_t differing = 0, at = 0;
	for (size_t i = 0; i < a.size(); ++i)
		if (a[i] != b[i]) {
			++differing;
			at = i;
		}
	ASSERT_EQ(differing, 1u);
	EXPECT_EQ(a[at].first, Series7::ConfRegType::CRC);
	EXPECT_EQ(b[at].first, Series7::ConfRegType::CMD);
	EXPECT_EQ(b[at].second,
	          static_cast<uint32_t>(xc7series::Command::RCRC));
	EXPECT_NE(a[at].second, 0u);
}

// Unit 6.2 step 5, the algorithm itself. The expected value is computed by an
// independent implementation (scripts/bit-packets.py, whose own CRC is checked
// against prjxray's crc_test.cc vectors) over the same seven register writes:
//   IDCODE<=0x1234, CMD<=NULL, MASK<=0x100, CTL0<=0x100, MASK<=0x400,
//   CTL0<=0x400, FAR<=0x03be0000
// with the leading CMD:RCRC resetting the accumulator and consuming its own
// write.
TEST(ConfigurationTest, StreamCrcMatchesAnIndependentImplementation) {
	using Reg = Series7::ConfRegType;
	Series7::ConfigurationPackage pkg;
	auto w1 = [&pkg](Reg reg, uint32_t v) {
		pkg.emplace_back(new ConfigurationPacketWithPayload<1, Reg>(
		    ConfigurationPacket<Reg>::Opcode::Write, reg, {v}));
	};
	pkg.emplace_back(new NopPacket<Reg>());
	w1(Reg::CMD, static_cast<uint32_t>(xc7series::Command::RCRC));
	pkg.emplace_back(new NopPacket<Reg>());
	pkg.emplace_back(new NopPacket<Reg>());
	w1(Reg::IDCODE, 0x1234);
	w1(Reg::CMD, static_cast<uint32_t>(xc7series::Command::NOP));
	w1(Reg::MASK, 0x100);
	w1(Reg::CTL0, 0x100);
	w1(Reg::MASK, 0x400);
	w1(Reg::CTL0, 0x400);
	w1(Reg::FAR, 0x03be0000);
	EXPECT_EQ(Configuration<Series7>::computeStreamCrc(pkg), 0x1d7601b3u);

	// A write to the CRC register RESETS the accumulator: appending one and
	// then repeating the same writes must give the same value again.
	w1(Reg::CRC, 0xDEADBEEF);
	w1(Reg::IDCODE, 0x1234);
	w1(Reg::CMD, static_cast<uint32_t>(xc7series::Command::NOP));
	w1(Reg::MASK, 0x100);
	w1(Reg::CTL0, 0x100);
	w1(Reg::MASK, 0x400);
	w1(Reg::CTL0, 0x400);
	w1(Reg::FAR, 0x03be0000);
	EXPECT_EQ(Configuration<Series7>::computeStreamCrc(pkg), 0x1d7601b3u);

	// And CMD:RCRC resets too, so the same again after one.
	w1(Reg::CMD, static_cast<uint32_t>(xc7series::Command::RCRC));
	w1(Reg::IDCODE, 0x1234);
	w1(Reg::CMD, static_cast<uint32_t>(xc7series::Command::NOP));
	w1(Reg::MASK, 0x100);
	w1(Reg::CTL0, 0x100);
	w1(Reg::MASK, 0x400);
	w1(Reg::CTL0, 0x400);
	w1(Reg::FAR, 0x03be0000);
	EXPECT_EQ(Configuration<Series7>::computeStreamCrc(pkg), 0x1d7601b3u);
}

// Unit 6.2 step 3. MASK/CTL0 appear THREE times, in PR #1492's exact triple,
// which the golden confirms: two pairs in the prologue (0x100/0x100 asserting
// GLUTMASK_B, then 0x400/0x400) and one in the epilogue (0x100/0x000). The
// prior corpus said they were "typically omitted".
TEST(ConfigurationTest, PartialPackageEmitsTheMaskCtl0Triple) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	auto statics = StaticOverTestPart();
	Frames<Series7>::Frames2Data frames;
	for (int m = 0; m < 4; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, m), 0xAB);
	auto blocks = Configuration<Series7>::createPartialPacketData(frames, statics,
	                                                              part, 0);
	Series7::ConfigurationPackage pkg;
	Configuration<Series7>::createPartialConfigurationPackage(
	    pkg, blocks, part, PartialStreamOptions());

	std::vector<std::pair<Series7::ConfRegType, uint32_t>> pairs;
	for (auto& e : RegSequence(pkg))
		if (e.first == Series7::ConfRegType::MASK ||
		    e.first == Series7::ConfRegType::CTL0)
			pairs.push_back(e);
	ASSERT_EQ(pairs.size(), 6u);
	EXPECT_EQ(pairs[0], std::make_pair(Series7::ConfRegType::MASK, 0x100u));
	EXPECT_EQ(pairs[1], std::make_pair(Series7::ConfRegType::CTL0, 0x100u));
	EXPECT_EQ(pairs[2], std::make_pair(Series7::ConfRegType::MASK, 0x400u));
	EXPECT_EQ(pairs[3], std::make_pair(Series7::ConfRegType::CTL0, 0x400u));
	EXPECT_EQ(pairs[4], std::make_pair(Series7::ConfRegType::MASK, 0x100u));
	EXPECT_EQ(pairs[5], std::make_pair(Series7::ConfRegType::CTL0, 0x0u));

	// The whole command/register sequence, in order, against the golden's.
	auto seq = RegSequence(pkg);
	using Reg = Series7::ConfRegType;
	std::vector<std::pair<Reg, uint32_t>> want{
	    {Reg::CMD, static_cast<uint32_t>(xc7series::Command::RCRC)},
	    {Reg::IDCODE, 0x1234},
	    {Reg::CMD, static_cast<uint32_t>(xc7series::Command::NOP)},
	    {Reg::MASK, 0x100},
	    {Reg::CTL0, 0x100},
	    {Reg::MASK, 0x400},
	    {Reg::CTL0, 0x400},
	    {Reg::CMD, static_cast<uint32_t>(xc7series::Command::WCFG)},
	    {Reg::FAR, Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, 0)},
	    {Reg::FDRI, 0},
	    {Reg::FDRI, 0xAB},
	    {Reg::MASK, 0x100},
	    {Reg::CTL0, 0x0},
	    {Reg::FAR, 0x03be0000},
	};
	ASSERT_GE(seq.size(), want.size() + 2);
	for (size_t i = 0; i < want.size(); ++i) {
		EXPECT_EQ(seq[i].first, want[i].first) << "at " << i;
		EXPECT_EQ(seq[i].second, want[i].second) << "at " << i;
	}
	EXPECT_EQ(seq[want.size()].first, Reg::CRC);
	EXPECT_EQ(seq[want.size() + 1].first, Reg::CMD);
	EXPECT_EQ(seq[want.size() + 1].second,
	          static_cast<uint32_t>(xc7series::Command::DESYNC));
	EXPECT_EQ(seq.size(), want.size() + 2);
}

// The trailing NOP count. 16 in every golden partial; PR #1492 emitted 400.
TEST(ConfigurationTest, PartialPackageEndsWithSixteenNops) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	auto statics = StaticOverTestPart();
	Frames<Series7>::Frames2Data frames;
	for (int m = 0; m < 4; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, m), 0xAB);
	auto blocks = Configuration<Series7>::createPartialPacketData(frames, statics,
	                                                              part, 0);
	Series7::ConfigurationPackage pkg;
	Configuration<Series7>::createPartialConfigurationPackage(
	    pkg, blocks, part, PartialStreamOptions());
	size_t trailing = 0;
	for (auto it = pkg.rbegin(); it != pkg.rend(); ++it) {
		if ((*it)->opcode() !=
		    ConfigurationPacket<Series7::ConfRegType>::Opcode::NOP)
			break;
		++trailing;
	}
	EXPECT_EQ(trailing, 16u);
}

// ============================================================================
// Unit X9. THE FULL BITSTREAM'S CRC.
//
// Everything above tests the PARTIAL path, which has computed a CRC since
// Unit 6.2. The full path never has: its finalization sequence carries two
// CMD:RCRC writes where a Vivado full bitstream carries two CRC writes, which
// is bit-for-bit Vivado's BITSTREAM.GENERAL.CRC=DISABLE substitution. H13
// measured what that costs on silicon -- one flipped configuration bit in a
// 3.8 MB stream, DONE=1, "No CRC error", and a design running 39x wrong.
// ============================================================================

// The substitution is TWO words per CRC and touches nothing else. Same shape
// of assertion as PartialPackageCrcDisableIsATwoWordSubstitution above, and
// the same reason: if emit_crc were a second code path rather than a
// substitution, this is where it would show.
TEST(ConfigurationTest, FullPackageCrcEmissionIsATwoWordSubstitution) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	Configuration<Series7>::PacketData data(404, 0xABu);

	Series7::ConfigurationPackage with_crc, without_crc;
	FullStreamOptions crc_on, crc_off;
	crc_on.emit_crc = true;
	Configuration<Series7>::createConfigurationPackage(with_crc, data, part,
	                                                   crc_on);
	Configuration<Series7>::createConfigurationPackage(without_crc, data,
	                                                   part, crc_off);

	ASSERT_EQ(with_crc.size(), without_crc.size());
	auto a = RegSequence(with_crc), b = RegSequence(without_crc);
	ASSERT_EQ(a.size(), b.size());
	std::vector<size_t> at;
	for (size_t i = 0; i < a.size(); ++i)
		if (a[i] != b[i])
			at.push_back(i);
	ASSERT_EQ(at.size(), 2u);
	for (size_t i : at) {
		EXPECT_EQ(a[i].first, Series7::ConfRegType::CRC);
		EXPECT_EQ(b[i].first, Series7::ConfRegType::CMD);
		EXPECT_EQ(b[i].second,
		          static_cast<uint32_t>(xc7series::Command::RCRC));
		EXPECT_NE(a[i].second, 0u);
	}
	// And the CRC-disabled default is unchanged by this work: three
	// CMD:RCRC writes, which is what every full bitstream in the tree was
	// built with.
	EXPECT_EQ(CountCmd(without_crc,
	                   static_cast<uint32_t>(xc7series::Command::RCRC)),
	          3u);
	EXPECT_EQ(CountCmd(with_crc,
	                   static_cast<uint32_t>(xc7series::Command::RCRC)),
	          1u);
}

// Each emitted CRC word is the accumulator over the packets that PRECEDE it,
// and a CRC write resets the accumulator -- so the second value covers only
// what follows the first. A writer that computed both over the whole stream,
// or both from zero at the start, would pass the substitution test above and
// fail here.
TEST(ConfigurationTest, FullPackageCrcIsTheAccumulatorAtEachWrite) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	Configuration<Series7>::PacketData data(404, 0xABu);
	Series7::ConfigurationPackage pkg;
	FullStreamOptions crc_on;
	crc_on.emit_crc = true;
	Configuration<Series7>::createConfigurationPackage(pkg, data, part,
	                                                   crc_on);

	size_t seen = 0;
	Series7::ConfigurationPackage prefix;
	for (auto& p : pkg) {
		const bool is_crc_write =
		    p->opcode() ==
		        ConfigurationPacket<Series7::ConfRegType>::Opcode::
		            Write &&
		    p->address() == Series7::ConfRegType::CRC &&
		    !p->data().empty();
		if (is_crc_write) {
			++seen;
			EXPECT_EQ(p->data()[0],
			          Configuration<Series7>::computeStreamCrc(
			              prefix))
			    << "CRC write #" << seen;
		}
		// The prefix is a NON-OWNING view: it borrows each packet's
		// span, and `pkg` outlives it.
		prefix.emplace_back(
		    new ConfigurationPacket<Series7::ConfRegType>(
		        p->header_type(), p->opcode(), p->address(),
		        p->data()));
	}
	EXPECT_EQ(seen, 2u);

	// The two values differ. If they did not, the reset would not be
	// happening and both would be measuring the same window.
	std::vector<uint32_t> values;
	for (auto& p : pkg)
		if (p->opcode() == ConfigurationPacket<
		                       Series7::ConfRegType>::Opcode::Write &&
		    p->address() == Series7::ConfRegType::CRC &&
		    !p->data().empty())
			values.push_back(p->data()[0]);
	ASSERT_EQ(values.size(), 2u);
	EXPECT_NE(values[0], values[1]);
}

// THE NEGATIVE CONTROL FOR THE WHOLE UNIT, in one test: change one word of
// frame data and the FIRST CRC must move. If it does not, the CRC covers
// nothing that a corrupted transfer can touch and emitting it would be
// ceremony. (The second CRC accumulates only post-CRC#1 writes, so it must
// NOT move -- which is the other half of the same claim.)
TEST(ConfigurationTest, FullPackageCrcMovesWhenOneFrameWordMoves) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	Configuration<Series7>::PacketData a(404, 0xABu);
	Configuration<Series7>::PacketData b = a;
	b[123] ^= 0x10u;  // one bit, in one word, of the frame data

	auto crcs = [&part](const Configuration<Series7>::PacketData& d) {
		Series7::ConfigurationPackage pkg;
		FullStreamOptions crc_on;
		crc_on.emit_crc = true;
		Configuration<Series7>::createConfigurationPackage(pkg, d, part,
		                                                   crc_on);
		std::vector<uint32_t> out;
		for (auto& p : pkg)
			if (p->opcode() ==
			        ConfigurationPacket<Series7::ConfRegType>::
			            Opcode::Write &&
			    p->address() == Series7::ConfRegType::CRC &&
			    !p->data().empty())
				out.push_back(p->data()[0]);
		return out;
	};
	auto ca = crcs(a), cb = crcs(b);
	ASSERT_EQ(ca.size(), 2u);
	ASSERT_EQ(cb.size(), 2u);
	EXPECT_NE(ca[0], cb[0]);
	EXPECT_EQ(ca[1], cb[1]);
}

// ============================================================================
// COR1, AND THE ONE BIT IN IT THAT DECIDES WHETHER ANYTHING ON THE DIE IS
// WATCHING CONFIGURATION MEMORY.
//
// The full-bitstream writer wrote COR1 = 0x00000000 from a literal, so
// COR1[8] RBCRC_EN -- UG470 ch. 8's continuous background readback, the only
// mechanism that can see an upset in a region that is running and is never
// reloaded -- could not be requested from this flow at all. These tests hold
// the two halves apart: the default emission must not have moved, and the
// override must reach exactly one word.
// ============================================================================

// A one-word substitution, and nothing else in the stream moves. Same shape as
// FullPackageCrcEmissionIsATwoWordSubstitution above, and the same reason: if
// the override were a second code path rather than a substituted operand, this
// is where it would show.
TEST(ConfigurationTest, FullPackageCor1IsAOneWordSubstitution) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	Configuration<Series7>::PacketData data(404, 0xABu);

	FullStreamOptions off, on;
	on.cor1 = xc7series::ConfigurationOptions1Value()
	              .SetContinuousReadbackCrc(true)
	              .SetReadbackCrcAction(
	                  xc7series::ConfigurationOptions1Value::
	                      ReadbackCrcAction::CorrectAndContinue);

	Series7::ConfigurationPackage with, without;
	Configuration<Series7>::createConfigurationPackage(with, data, part,
	                                                   on);
	Configuration<Series7>::createConfigurationPackage(without, data, part,
	                                                   off);

	ASSERT_EQ(with.size(), without.size());
	auto a = RegSequence(with), b = RegSequence(without);
	ASSERT_EQ(a.size(), b.size());
	std::vector<size_t> at;
	for (size_t i = 0; i < a.size(); ++i)
		if (a[i] != b[i])
			at.push_back(i);
	ASSERT_EQ(at.size(), 1u);
	EXPECT_EQ(a[at[0]].first, Series7::ConfRegType::COR1);
	EXPECT_EQ(b[at[0]].first, Series7::ConfRegType::COR1);
	// 0x00010100: bit 8 RBCRC_EN, bits 16:15 = 0b10 Correct_And_Continue.
	EXPECT_EQ(a[at[0]].second, 0x00010100u);
	EXPECT_EQ(b[at[0]].second, 0x00000000u);
}

// The DEFAULT is the artefact-preserving half, and it is asserted on its own
// because every recorded payload hash in the consuming project identifies a
// full bitstream by exactly this word. Exactly one COR1 write, value zero.
TEST(ConfigurationTest, FullPackageCor1DefaultsToZero) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	Configuration<Series7>::PacketData data(404, 0xABu);
	Series7::ConfigurationPackage pkg;
	Configuration<Series7>::createConfigurationPackage(pkg, data, part,
	                                                   FullStreamOptions());

	size_t writes = 0;
	for (auto& e : RegSequence(pkg))
		if (e.first == Series7::ConfRegType::COR1) {
			++writes;
			EXPECT_EQ(e.second, 0x00000000u);
		}
	EXPECT_EQ(writes, 1u);
}

// UG470 Table 5-33, field by field. Written as literal expected words rather
// than as an expression over the same shifts the builder uses, because a test
// that recomputes the implementation proves only that it is self-consistent.
//
// RBCRC_ACTION IS THE ONE THAT CAN BE WRONG SILENTLY: the encoding is
// 00 Continue, 01 Halt, 11 Correct_And_Halt, 10 Correct_And_Continue, so the
// two "correct" actions are NOT in the order a reader assumes.
TEST(ConfigurationTest, Cor1FieldsMatchUg470Table533) {
	using V = xc7series::ConfigurationOptions1Value;
	using Action = V::ReadbackCrcAction;

	EXPECT_EQ(static_cast<uint32_t>(V()), 0x00000000u);

	EXPECT_EQ(static_cast<uint32_t>(V().SetContinuousReadbackCrc(true)),
	          0x00000100u);
	EXPECT_EQ(
	    static_cast<uint32_t>(V().SetReadbackCrcErrorPinDisabled(true)),
	    0x00000200u);
	EXPECT_EQ(static_cast<uint32_t>(V().SetPersistDeassertAtDesync(true)),
	          0x00020000u);

	EXPECT_EQ(
	    static_cast<uint32_t>(V().SetReadbackCrcAction(Action::Continue)),
	    0x00000000u);
	EXPECT_EQ(static_cast<uint32_t>(V().SetReadbackCrcAction(Action::Halt)),
	          0x00008000u);
	EXPECT_EQ(static_cast<uint32_t>(
	              V().SetReadbackCrcAction(Action::CorrectAndContinue)),
	          0x00010000u);
	EXPECT_EQ(static_cast<uint32_t>(
	              V().SetReadbackCrcAction(Action::CorrectAndHalt)),
	          0x00018000u);

	EXPECT_EQ(static_cast<uint32_t>(
	              V().SetBpiFirstReadCycle(V::BpiFirstReadCycle::Cclk4)),
	          0x0000000Cu);
	EXPECT_EQ(
	    static_cast<uint32_t>(V().SetBpiPageSize(V::BpiPageSize::Bytes8)),
	    0x00000002u);

	// The fields are disjoint: setting all six leaves each one's bits
	// where they were. A builder whose shifts overlapped would pass every
	// single-field assertion above and fail here.
	//
	// 0x0003830E is the OR of the six words asserted individually above:
	// 0x20000 | 0x18000 | 0x200 | 0x100 | 0xC | 0x2. It is written out
	// rather than expressed as that OR because the point is to check the
	// builder against the register, not against itself -- and the first
	// draft of this line said 0x0001A30E, which is that same OR done
	// wrong by hand and is what this assertion caught.
	EXPECT_EQ(static_cast<uint32_t>(
	              V().SetPersistDeassertAtDesync(true)
	                  .SetReadbackCrcAction(Action::CorrectAndHalt)
	                  .SetReadbackCrcErrorPinDisabled(true)
	                  .SetContinuousReadbackCrc(true)
	                  .SetBpiFirstReadCycle(V::BpiFirstReadCycle::Cclk4)
	                  .SetBpiPageSize(V::BpiPageSize::Bytes8)),
	          0x0003830Eu);
}

// The knob is Series7-only and says so by refusing. UltraScale's COR1 default
// in this writer is 0x00400000 with a UG570 layout nobody here has measured;
// accepting a 7-series field pattern into it would emit a plausible-looking
// guess at exit 0, which is the defect class this whole file exists to close.
TEST(ConfigurationTest, Cor1OverrideIsRefusedOnUltraScale) {
	// UltraScale derives from Series7, so UltraScale::Part IS
	// xc7series::Part and the same fixture serves. Only the register
	// literals and the frame width differ, and neither is exercised here.
	auto part = absl::optional<UltraScale::Part>(MakeTestPart());
	Configuration<UltraScale>::PacketData data(123, 0xABu);

	FullStreamOptions on;
	on.cor1 = 0x100u;
	UltraScale::ConfigurationPackage pkg;
	EXPECT_THROW(Configuration<UltraScale>::createConfigurationPackage(
	                 pkg, data, part, on),
	             std::runtime_error);

	// The other direction: with the option unset the same call is fine and
	// still writes UltraScale's own literal. A refusal-only test would pass
	// on a writer that threw unconditionally.
	UltraScale::ConfigurationPackage ok;
	EXPECT_NO_THROW(Configuration<UltraScale>::createConfigurationPackage(
	    ok, data, part, FullStreamOptions()));
	size_t writes = 0;
	for (auto& p : ok)
		if (p->opcode() ==
		        ConfigurationPacket<
		            UltraScale::ConfRegType>::Opcode::Write &&
		    p->address() == UltraScale::ConfRegType::COR1 &&
		    !p->data().empty()) {
			++writes;
			EXPECT_EQ(p->data()[0], 0x400000u);
		}
	EXPECT_EQ(writes, 1u);
}

namespace {

ConfigurationPacket<Series7::ConfRegType> WritePacket(
    Series7::ConfRegType reg,
    const std::vector<uint32_t>& data,
    unsigned int header_type = 0x1) {
	return ConfigurationPacket<Series7::ConfRegType>(
	    header_type,
	    ConfigurationPacket<Series7::ConfRegType>::Opcode::Write, reg,
	    absl::MakeConstSpan(data));
}

xc7series::Part MakeThreeFramePart() {
	std::vector<xc7series::FrameAddress> addresses{0x4560, 0x4561, 0x4562};
	return xc7series::Part(0x1234, addresses);
}

std::deque<std::vector<uint32_t>> frame_runs;

const std::vector<uint32_t>& FrameRun(std::initializer_list<uint32_t> fills) {
	frame_runs.emplace_back();
	std::vector<uint32_t>& run = frame_runs.back();
	for (uint32_t fill : fills)
		run.insert(run.end(), Series7::words_per_frame, fill);
	return run;
}

}  // namespace

TEST(ConfigurationTest, PaddingAfterTheLastFrameDoesNotOverwriteIt) {
	auto part = MakeThreeFramePart();

	std::vector<uint32_t> idcode{0x1234};
	std::vector<uint32_t> wcfg{0x1};
	std::vector<uint32_t> far{0x4561};
	const auto& payload = FrameRun({0xAA, 0xBB, 0x00});
	const auto& padding = FrameRun({0xCC});

	std::vector<ConfigurationPacket<Series7::ConfRegType>> packets{
	    WritePacket(Series7::ConfRegType::IDCODE, idcode),
	    WritePacket(Series7::ConfRegType::CMD, wcfg),
	    WritePacket(Series7::ConfRegType::FAR, far),
	    WritePacket(Series7::ConfRegType::FDRI, payload),
	    WritePacket(Series7::ConfRegType::FDRI, padding),
	};

	auto config = Configuration<Series7>::InitWithPackets(part, packets);
	ASSERT_TRUE(config);
	EXPECT_EQ(config->frames().size(), 2u);
	EXPECT_EQ(config->frames().at(0x4561),
	          std::vector<uint32_t>(Series7::words_per_frame, 0xAA));
	EXPECT_EQ(config->frames().at(0x4562),
	          std::vector<uint32_t>(Series7::words_per_frame, 0xBB));
}

TEST(ConfigurationTest, NewWriteAfterTheLastFrameIsApplied) {
	auto part = MakeThreeFramePart();

	std::vector<uint32_t> idcode{0x1234};
	std::vector<uint32_t> wcfg{0x1};
	std::vector<uint32_t> far_last{0x4562};
	std::vector<uint32_t> far_first{0x4560};

	std::vector<ConfigurationPacket<Series7::ConfRegType>> packets{
	    WritePacket(Series7::ConfRegType::IDCODE, idcode),
	    WritePacket(Series7::ConfRegType::CMD, wcfg),
	    WritePacket(Series7::ConfRegType::FAR, far_last),
	    WritePacket(Series7::ConfRegType::FDRI, FrameRun({0xAA, 0x00})),
	    WritePacket(Series7::ConfRegType::FDRI, FrameRun({0xCC})),
	    WritePacket(Series7::ConfRegType::FAR, far_first),
	    WritePacket(Series7::ConfRegType::CMD, wcfg),
	    WritePacket(Series7::ConfRegType::FDRI, FrameRun({0xDD})),
	    WritePacket(Series7::ConfRegType::FAR, far_last),
	    WritePacket(Series7::ConfRegType::FDRI, FrameRun({0xEE, 0x00})),
	    WritePacket(Series7::ConfRegType::FDRI, FrameRun({0xCC})),
	};

	auto config = Configuration<Series7>::InitWithPackets(part, packets);
	ASSERT_TRUE(config);
	EXPECT_EQ(config->frames().size(), 2u);
	EXPECT_EQ(config->frames().at(0x4560),
	          std::vector<uint32_t>(Series7::words_per_frame, 0xDD));
	EXPECT_EQ(config->frames().at(0x4562),
	          std::vector<uint32_t>(Series7::words_per_frame, 0xEE));
}

TEST(ConfigurationTest, TypeTwoContinuationAfterEndIsDropped) {
	auto part = MakeThreeFramePart();
	std::vector<uint32_t> idcode{0x1234}, wcfg{0x1}, far{0x4560}, empty;
	const auto& payload = FrameRun({0xA0, 0xA1, 0xA2});
	const auto& padding = FrameRun({0xCC, 0xCD});
	std::vector<ConfigurationPacket<Series7::ConfRegType>> p{
	    WritePacket(Series7::ConfRegType::IDCODE, idcode),
	    WritePacket(Series7::ConfRegType::FAR, far),
	    WritePacket(Series7::ConfRegType::CMD, wcfg),
	    WritePacket(Series7::ConfRegType::FDRI, empty),
	    WritePacket(Series7::ConfRegType::FDRI, payload, 0x2),
	    WritePacket(Series7::ConfRegType::FDRI, empty),
	    WritePacket(Series7::ConfRegType::FDRI, padding, 0x2),
	};
	auto c = Configuration<Series7>::InitWithPackets(part, p);
	ASSERT_TRUE(c);
	ASSERT_EQ(c->frames().size(), 3u);
	EXPECT_EQ(c->frames().at(0x4560)[0], 0xA0u);
	EXPECT_EQ(c->frames().at(0x4561)[0], 0xA1u);
	EXPECT_EQ(c->frames().at(0x4562)[0], 0xA2u);
}

TEST(ConfigurationTest, OnePacketRunningPastTheEndKeepsEveryRealFrame) {
	auto part = MakeThreeFramePart();
	std::vector<uint32_t> idcode{0x1234}, wcfg{0x1}, far{0x4561};
	const auto& payload = FrameRun({0xB1, 0xB2, 0xEE, 0xEF, 0xF0});
	std::vector<ConfigurationPacket<Series7::ConfRegType>> p{
	    WritePacket(Series7::ConfRegType::IDCODE, idcode),
	    WritePacket(Series7::ConfRegType::CMD, wcfg),
	    WritePacket(Series7::ConfRegType::FAR, far),
	    WritePacket(Series7::ConfRegType::FDRI, payload),
	};
	auto c = Configuration<Series7>::InitWithPackets(part, p);
	ASSERT_TRUE(c);
	ASSERT_EQ(c->frames().size(), 2u);
	EXPECT_EQ(c->frames().at(0x4561)[0], 0xB1u);
	EXPECT_EQ(c->frames().at(0x4562)[0], 0xB2u);
}

TEST(ConfigurationTest, PerFrameCrcProgressFarsDoNotReopenTheLastFrame) {
	auto part = MakeThreeFramePart();
	std::vector<uint32_t> idcode{0x1234}, wcfg{0x1}, mask{0xFFFFFFFF},
	    ctl1{1u << 21}, crc{0xDEADBEEF};
	std::vector<uint32_t> f0{0x4560}, f1{0x4561}, f2{0x4562};
	std::vector<ConfigurationPacket<Series7::ConfRegType>> p{
	    WritePacket(Series7::ConfRegType::IDCODE, idcode),
	    WritePacket(Series7::ConfRegType::MASK, mask),
	    WritePacket(Series7::ConfRegType::CTL1, ctl1),
	    WritePacket(Series7::ConfRegType::FAR, f0),
	    WritePacket(Series7::ConfRegType::CMD, wcfg),
	    WritePacket(Series7::ConfRegType::FDRI, FrameRun({0xC0})),
	    WritePacket(Series7::ConfRegType::CRC, crc),
	    WritePacket(Series7::ConfRegType::FAR, f1),
	    WritePacket(Series7::ConfRegType::FDRI, FrameRun({0xC1})),
	    WritePacket(Series7::ConfRegType::CRC, crc),
	    WritePacket(Series7::ConfRegType::FAR, f2),
	    WritePacket(Series7::ConfRegType::FDRI, FrameRun({0xC2})),
	    WritePacket(Series7::ConfRegType::CRC, crc),
	    WritePacket(Series7::ConfRegType::FAR, f2),
	    WritePacket(Series7::ConfRegType::FDRI, FrameRun({0x00})),
	    WritePacket(Series7::ConfRegType::CRC, crc),
	};
	auto c = Configuration<Series7>::InitWithPackets(part, p);
	ASSERT_TRUE(c);
	ASSERT_EQ(c->frames().size(), 3u);
	EXPECT_EQ(c->frames().at(0x4560)[0], 0xC0u);
	EXPECT_EQ(c->frames().at(0x4561)[0], 0xC1u);
	EXPECT_EQ(c->frames().at(0x4562)[0], 0xC2u);
}

TEST(ConfigurationTest, BareWcfgAfterTheEndRewritesTheLastFrame) {
	auto part = MakeThreeFramePart();
	std::vector<uint32_t> idcode{0x1234}, wcfg{0x1}, far{0x4562};
	std::vector<ConfigurationPacket<Series7::ConfRegType>> p{
	    WritePacket(Series7::ConfRegType::IDCODE, idcode),
	    WritePacket(Series7::ConfRegType::CMD, wcfg),
	    WritePacket(Series7::ConfRegType::FAR, far),
	    WritePacket(Series7::ConfRegType::FDRI, FrameRun({0xD0, 0x00})),
	    WritePacket(Series7::ConfRegType::CMD, wcfg),
	    WritePacket(Series7::ConfRegType::FDRI, FrameRun({0xD1, 0x00})),
	};
	auto c = Configuration<Series7>::InitWithPackets(part, p);
	ASSERT_TRUE(c);
	EXPECT_EQ(c->frames().at(0x4562)[0], 0xD1u);
}

TEST(ConfigurationTest, MfwrAfterTheEndReplaysWhatFdriLastCarried) {
	auto part = MakeThreeFramePart();
	std::vector<uint32_t> idcode{0x1234}, wcfg{0x1}, far{0x4562},
	    far0{0x4560}, mfw{0x2}, mfwr{0, 0, 0, 0};
	std::vector<ConfigurationPacket<Series7::ConfRegType>> p{
	    WritePacket(Series7::ConfRegType::IDCODE, idcode),
	    WritePacket(Series7::ConfRegType::CMD, wcfg),
	    WritePacket(Series7::ConfRegType::FAR, far),
	    WritePacket(Series7::ConfRegType::FDRI, FrameRun({0xE2})),
	    WritePacket(Series7::ConfRegType::FDRI, FrameRun({0xE9})),
	    WritePacket(Series7::ConfRegType::CMD, mfw),
	    WritePacket(Series7::ConfRegType::FAR, far0),
	    WritePacket(Series7::ConfRegType::MFWR, mfwr),
	};
	auto c = Configuration<Series7>::InitWithPackets(part, p);
	ASSERT_TRUE(c);
	EXPECT_EQ(c->frames().at(0x4562)[0], 0xE2u);
	EXPECT_EQ(c->frames().at(0x4560)[0], 0xE9u);
}

TEST(ConfigurationTest, BlockRamIsTheLastBlockTypeAndClbEndIsNotTheEnd) {
	std::vector<xc7series::FrameAddress> a = {
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, false, 0,
	                            0, 0),
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, true, 0,
	                            0, 0),
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, true, 1,
	                            0, 0),
	    xc7series::FrameAddress(xc7series::BlockType::BLOCK_RAM, false, 0,
	                            0, 0),
	    xc7series::FrameAddress(xc7series::BlockType::BLOCK_RAM, false, 1,
	                            0, 0)};
	auto part = absl::optional<xc7series::Part>(xc7series::Part(0x1234, a));
	Frames<Series7> frames;
	uint32_t fill = 0x10;
	for (auto& addr : a)
		frames.getFrames().emplace(addr,
		                           std::vector<uint32_t>(101, fill++));
	auto data = Configuration<Series7>::createType2ConfigurationPacketData(
	    frames.getFrames(), part);
	std::vector<uint32_t> idcode{0x1234}, wcfg{0x1}, far{0x0}, empty;
	const auto& trailing = FrameRun({0x99, 0x98});
	std::vector<ConfigurationPacket<Series7::ConfRegType>> p{
	    WritePacket(Series7::ConfRegType::IDCODE, idcode),
	    WritePacket(Series7::ConfRegType::FAR, far),
	    WritePacket(Series7::ConfRegType::CMD, wcfg),
	    WritePacket(Series7::ConfRegType::FDRI, empty),
	    WritePacket(Series7::ConfRegType::FDRI, data, 0x2),
	    WritePacket(Series7::ConfRegType::FDRI, trailing),
	};
	auto c = Configuration<Series7>::InitWithPackets(*part, p);
	ASSERT_TRUE(c);
	ASSERT_EQ(c->frames().size(), 5u);
	fill = 0x10;
	for (auto& addr : a)
		EXPECT_EQ(c->frames().at(addr)[0], fill++);
}

TEST(ConfigurationTest, PartialPackageEndingAtTheDeviceEndRoundTrips) {
	std::vector<xc7series::FrameAddress> a = {
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, false, 0,
	                            0, 0),
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, false, 0,
	                            0, 1),
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, false, 0,
	                            0, 2),
	    xc7series::FrameAddress(xc7series::BlockType::CLB_IO_CLK, false, 0,
	                            0, 3)};
	auto part = absl::optional<xc7series::Part>(xc7series::Part(0x1234, a));
	Frames<Series7> f1, f2, flush;
	f1.getFrames().emplace(a[2], std::vector<uint32_t>(101, 0x21));
	f1.getFrames().emplace(a[3], std::vector<uint32_t>(101, 0x22));
	f2.getFrames().emplace(a[0], std::vector<uint32_t>(101, 0x30));
	flush.getFrames().emplace(a[1], std::vector<uint32_t>(101, 0x31));
	auto b1 = Configuration<Series7>::createPartialPacketData(
	    f1.getFrames(), flush.getFrames(), part, 2);
	auto b2 = Configuration<Series7>::createPartialPacketData(
	    f2.getFrames(), flush.getFrames(), part, 2);
	Configuration<Series7>::PartialPacketData blocks = b1;
	blocks.insert(blocks.end(), b2.begin(), b2.end());
	blocks.insert(blocks.end(), b1.begin(), b1.end());
	Series7::ConfigurationPackage pkg;
	PartialStreamOptions opts;
	Configuration<Series7>::createPartialConfigurationPackage(pkg, blocks,
	                                                          part, opts);
	std::vector<ConfigurationPacket<Series7::ConfRegType>> p;
	for (auto& pk : pkg)
		p.emplace_back(pk->header_type(), pk->opcode(), pk->address(),
		               pk->data());
	auto c = Configuration<Series7>::InitWithPackets(*part, p);
	ASSERT_TRUE(c);
	EXPECT_EQ(c->frames().at(a[3])[0], 0x22u);
	EXPECT_EQ(c->frames().at(a[2])[0], 0x21u);
	EXPECT_EQ(c->frames().at(a[0])[0], 0x30u);
	EXPECT_EQ(c->frames().at(a[1])[0], 0x31u);
}
