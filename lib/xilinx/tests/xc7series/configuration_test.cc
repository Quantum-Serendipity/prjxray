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
#include <iostream>
#include <vector>

#include <absl/types/span.h>
#include <gtest/gtest.h>
#include <prjxray/memory_mapped_file.h>
#include <prjxray/xilinx/architectures.h>
#include <prjxray/xilinx/bitstream_reader.h>
#include <prjxray/xilinx/configuration.h>
#include <prjxray/xilinx/configuration_packet.h>
#include <prjxray/xilinx/configuration_register.h>
#include <prjxray/xilinx/frames.h>
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

}  // namespace

// Derive-from-input: the emitted frame set is the input frame set, and a gap
// in the input becomes a new FAR block rather than a run of zero frames.
TEST(ConfigurationTest, PartialPacketDataDerivesFromInput) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
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
	    frames, part, /*pad_frames=*/0);

	// Three runs: [clb r0 c0 m2..3], [clb r0 c1 m2..3], [bram r0 c0 m0..3].
	ASSERT_EQ(blocks.size(), 3u);
	EXPECT_EQ(blocks[0].first,
	          Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, 2));
	EXPECT_EQ(blocks[0].second.size(), 2u * 101);
	EXPECT_EQ(blocks[1].first,
	          Addr(xc7series::BlockType::CLB_IO_CLK, 0, 1, 2));
	EXPECT_EQ(blocks[1].second.size(), 2u * 101);
	EXPECT_EQ(blocks[2].first,
	          Addr(xc7series::BlockType::BLOCK_RAM, 0, 0, 0));
	EXPECT_EQ(blocks[2].second.size(), 4u * 101);

	// Total words == total input frames. Nothing was invented.
	size_t words = 0;
	for (auto& b : blocks)
		words += b.second.size();
	EXPECT_EQ(words, frames.size() * 101);

	// And the payloads are the caller's, in order.
	EXPECT_EQ(blocks[0].second.front(), 0xAAu);
	EXPECT_EQ(blocks[0].second.at(101), 0xBBu);
	EXPECT_EQ(blocks[1].second.front(), 0xCCu);

	// The frame-set check agrees, walking forward from each FAR.
	EXPECT_EQ(Configuration<Series7>::checkPartialFrameSet(blocks, frames,
	                                                       part, 0),
	          frames.size());
}

// A run that crosses a row boundary is split, because the hardware inserts two
// pad frames there and a block spanning one would need padding in the middle.
TEST(ConfigurationTest, PartialPacketDataSplitsAtRowBoundary) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	Frames<Series7>::Frames2Data frames;
	for (int m = 0; m < 4; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 0, 1, m), 0x11);
	for (int m = 0; m < 4; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 1, 0, m), 0x22);

	auto blocks = Configuration<Series7>::createPartialPacketData(
	    frames, part, /*pad_frames=*/0);
	ASSERT_EQ(blocks.size(), 2u);
	EXPECT_EQ(blocks[0].first,
	          Addr(xc7series::BlockType::CLB_IO_CLK, 0, 1, 0));
	EXPECT_EQ(blocks[1].first,
	          Addr(xc7series::BlockType::CLB_IO_CLK, 1, 0, 0));
	EXPECT_EQ(Configuration<Series7>::checkPartialFrameSet(blocks, frames,
	                                                       part, 0),
	          8u);
}

// --pad_frames defaults to 0. When it is raised, pads go ONLY on a block that
// ends at a row/bus boundary -- which is where the device has dummy frame
// positions and where Vivado puts them. A block that stops mid-row gets none,
// because there a pad would land on a live static frame.
TEST(ConfigurationTest, PartialPacketDataPadsOnlyAtRowEnds) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());

	// Block ending MID-ROW: col 0 minors 0-2 of a 4-frame column.
	Frames<Series7>::Frames2Data midrow;
	for (int m = 0; m < 3; ++m)
		AddFrame(&midrow,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, m), 0x33);
	auto a = Configuration<Series7>::createPartialPacketData(midrow, part, 0);
	auto b = Configuration<Series7>::createPartialPacketData(midrow, part, 2);
	ASSERT_EQ(a.size(), 1u);
	ASSERT_EQ(b.size(), 1u);
	EXPECT_EQ(a[0].second.size(), 3u * 101);
	EXPECT_EQ(b[0].second.size(), 3u * 101);  // no pad: mid-row

	// Block ending at a ROW END: all of CLB row 0, cols 0 and 1.
	Frames<Series7>::Frames2Data rowend;
	for (int col = 0; col < 2; ++col)
		for (int m = 0; m < 4; ++m)
			AddFrame(&rowend,
			         Addr(xc7series::BlockType::CLB_IO_CLK, 0, col,
			              m),
			         0x44);
	auto none = Configuration<Series7>::createPartialPacketData(rowend, part,
	                                                            0);
	auto padded = Configuration<Series7>::createPartialPacketData(rowend,
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
	                                                       part, 0),
	          8u);
	EXPECT_EQ(Configuration<Series7>::checkPartialFrameSet(padded, rowend,
	                                                       part, 2),
	          8u);
	EXPECT_THROW(Configuration<Series7>::checkPartialFrameSet(padded, rowend,
	                                                          part, 0),
	             std::runtime_error);
}

// The check must discriminate, not merely pass: a block whose length claims
// more frames than the caller asked for is rejected, naming the first address
// outside the input.
TEST(ConfigurationTest, PartialFrameSetCheckRejectsALeak) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	Frames<Series7>::Frames2Data frames;
	for (int m = 0; m < 2; ++m)
		AddFrame(&frames,
		         Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, m), 0x44);

	auto blocks = Configuration<Series7>::createPartialPacketData(frames,
	                                                              part, 0);
	ASSERT_EQ(Configuration<Series7>::checkPartialFrameSet(blocks, frames,
	                                                       part, 0),
	          2u);
	// Grow the block by one frame without telling the caller -- exactly
	// what the zero-fill design did, 2,032 times over.
	blocks[0].second.resize(3 * 101, 0);
	EXPECT_THROW(Configuration<Series7>::checkPartialFrameSet(blocks, frames,
	                                                          part, 0),
	             std::runtime_error);
	// ...and a block that drops a frame is rejected too.
	blocks[0].second.resize(1 * 101, 0);
	EXPECT_THROW(Configuration<Series7>::checkPartialFrameSet(blocks, frames,
	                                                          part, 0),
	             std::runtime_error);
}

// An address the part does not have is an error, not a silent walk off the end
// of the device. This is one of the four inputs that used to spin at 100% CPU.
TEST(ConfigurationTest, PartialPacketDataRejectsInvalidAddress) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
	Frames<Series7>::Frames2Data frames;
	AddFrame(&frames, Addr(xc7series::BlockType::CLB_IO_CLK, 0, 0, 0), 0x55);
	AddFrame(&frames, Addr(xc7series::BlockType::CLB_IO_CLK, 7, 9, 0), 0x66);
	EXPECT_THROW(
	    Configuration<Series7>::createPartialPacketData(frames, part, 0),
	    std::runtime_error);
}

// Antmicro's original test, adapted to the derive-from-input API: the package
// still carries one FAR write per block, in ascending address order, with the
// CLB block before the BRAM one.
TEST(ConfigurationTest, CreatePartialConfigurationPackage) {
	auto part = absl::optional<xc7series::Part>(MakeTestPart());
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
	    frames, part, 0);
	Series7::ConfigurationPackage partial_configuration_package;
	Configuration<Series7>::createPartialConfigurationPackage(
	    partial_configuration_package, cfg_packets, part);

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
	EXPECT_EQ(fdri_words, frames.size() * 101);
}
