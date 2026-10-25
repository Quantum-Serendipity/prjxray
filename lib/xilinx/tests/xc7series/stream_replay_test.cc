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
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <prjxray/memory_mapped_file.h>
#include <prjxray/xilinx/architectures.h>
#include <prjxray/xilinx/bitstream_writer.h>
#include <prjxray/xilinx/configuration.h>
#include <prjxray/xilinx/configuration_packet.h>
#include <prjxray/xilinx/xc7series/command.h>
#include <prjxray/xilinx/xc7series/stream_replay.h>

using namespace prjxray::xilinx;

namespace {

constexpr uint32_t kReplayTestIdcode = 0x1234;

xc7series::Part ReplayTestPart() {
	std::vector<xc7series::FrameAddress> addresses;
	auto add = [&addresses](xc7series::BlockType blk, int row, int col,
	                        int frames) {
		for (int m = 0; m < frames; ++m)
			addresses.push_back(
			    xc7series::FrameAddress(blk, false, row, col, m));
	};
	add(xc7series::BlockType::CLB_IO_CLK, 0, 0, 4);
	add(xc7series::BlockType::CLB_IO_CLK, 0, 1, 4);
	add(xc7series::BlockType::CLB_IO_CLK, 1, 0, 4);
	add(xc7series::BlockType::BLOCK_RAM, 0, 0, 4);
	add(xc7series::BlockType::BLOCK_RAM, 0, 1, 4);
	return xc7series::Part(kReplayTestIdcode, addresses);
}

uint32_t Far(xc7series::BlockType blk, int row, int col, int minor) {
	return static_cast<uint32_t>(
	    xc7series::FrameAddress(blk, false, row, col, minor));
}

std::vector<uint32_t> Frame(uint32_t fill) {
	return std::vector<uint32_t>(101, fill);
}

Frames<Series7>::Frames2Data AllFrames(const xc7series::Part& part,
                                       const std::vector<uint32_t>& fills) {
	Frames<Series7>::Frames2Data frames;
	absl::optional<xc7series::FrameAddress> a(xc7series::FrameAddress(0));
	size_t i = 0;
	while (a) {
		frames.emplace(*a, Frame(fills.at(i++)));
		a = part.GetNextFrameAddress(*a);
	}
	EXPECT_EQ(i, fills.size());
	return frames;
}

std::vector<uint8_t> Serialize(const Series7::ConfigurationPackage& packets) {
	std::vector<uint8_t> bytes;
	for (uint32_t word : BitstreamWriter<Series7>(packets))
		for (int b = 3; b >= 0; --b)
			bytes.push_back((word >> (8 * b)) & 0xff);
	return bytes;
}

class Stream {
       public:
	explicit Stream(bool idcode = true) {
		if (idcode)
			Write(12, {kReplayTestIdcode});
	}
	Stream& Write(uint32_t reg, std::vector<uint32_t> payload) {
		words_.push_back((1u << 29) | (2u << 27) | (reg << 13) |
		                 static_cast<uint32_t>(payload.size()));
		words_.insert(words_.end(), payload.begin(), payload.end());
		return *this;
	}
	Stream& Fdri(const std::vector<uint32_t>& payload) {
		words_.push_back((1u << 29) | (2u << 27) | (2u << 13));
		words_.push_back((2u << 29) | (2u << 27) |
		                 static_cast<uint32_t>(payload.size()));
		words_.insert(words_.end(), payload.begin(), payload.end());
		return *this;
	}
	Stream& Nop() {
		words_.push_back(0x20000000);
		return *this;
	}
	Stream& Cmd(xc7series::Command c) {
		return Write(4, {static_cast<uint32_t>(c)});
	}
	Stream& FarWrite(uint32_t a) { return Write(1, {a}); }
	std::vector<uint8_t> Bytes() const {
		std::vector<uint8_t> bytes = {0xff, 0xff, 0xff, 0xff,
		                              0xaa, 0x99, 0x55, 0x66};
		for (uint32_t w : words_)
			for (int b = 3; b >= 0; --b)
				bytes.push_back((w >> (8 * b)) & 0xff);
		return bytes;
	}

       private:
	std::vector<uint32_t> words_;
};

std::vector<uint32_t> Concat(std::vector<std::vector<uint32_t>> frames) {
	std::vector<uint32_t> out;
	for (const auto& f : frames)
		out.insert(out.end(), f.begin(), f.end());
	return out;
}

void ExpectEachFrameCommittedOnce(const xc7series::StreamReplay& replay,
                                  const Frames<Series7>::Frames2Data& frames) {
	std::map<uint32_t, std::vector<std::vector<uint32_t>>> got;
	for (const auto& c : replay.committed)
		got[c.address].push_back(c.words);
	EXPECT_EQ(got.size(), frames.size());
	for (const auto& f : frames) {
		auto it = got.find(static_cast<uint32_t>(f.first));
		ASSERT_NE(it, got.end())
		    << std::hex << static_cast<uint32_t>(f.first);
		ASSERT_EQ(it->second.size(), 1u)
		    << std::hex << static_cast<uint32_t>(f.first);
		EXPECT_EQ(it->second[0], f.second)
		    << std::hex << static_cast<uint32_t>(f.first);
	}
	EXPECT_TRUE(replay.discarded.empty());
}

const std::vector<uint32_t> kZeroRunFills = {
    0xA1, 0,    0,    0xB1, 0, 0, 0,    0,    0xC1, 0xC2,
    0xC3, 0xC4, 0xD1, 0xD2, 0, 0, 0xE1, 0xE2, 0,    0};

}  // namespace

TEST(StreamReplayTest, FullStreamCommitsEveryFrameOnce) {
	auto part = absl::optional<xc7series::Part>(ReplayTestPart());
	std::vector<uint32_t> fills;
	for (uint32_t i = 0; i < 20; ++i)
		fills.push_back(0x100 + i);
	auto frames = AllFrames(*part, fills);
	auto data = Configuration<Series7>::createType2ConfigurationPacketData(
	    frames, part);
	Series7::ConfigurationPackage packets;
	Configuration<Series7>::createConfigurationPackage(packets, data, part,
	                                                   FullStreamOptions());
	xc7series::StreamReplay replay;
	std::string error;
	ASSERT_TRUE(xc7series::ReplayConfigurationStream(
	    Serialize(packets), *part, &replay, &error))
	    << error;
	EXPECT_TRUE(replay.desynced);
	ExpectEachFrameCommittedOnce(replay, frames);
	EXPECT_EQ(replay.mfw_replays, 0u);
}

TEST(StreamReplayTest, CompressedStreamCommitsEveryFrameOnce) {
	auto part = absl::optional<xc7series::Part>(ReplayTestPart());
	auto frames = AllFrames(*part, kZeroRunFills);
	auto blocks =
	    Configuration<Series7>::createCompressedFrameData(frames, part);
	ASSERT_EQ(Configuration<Series7>::checkCompressedFrameSet(blocks,
	                                                          frames, part),
	          20u);
	FullStreamOptions options;
	options.compressed = &blocks;
	Series7::ConfigurationPackage packets;
	Configuration<Series7>::createConfigurationPackage(
	    packets, Configuration<Series7>::PacketData(), part, options);
	xc7series::StreamReplay replay;
	std::string error;
	ASSERT_TRUE(xc7series::ReplayConfigurationStream(
	    Serialize(packets), *part, &replay, &error))
	    << error;
	ExpectEachFrameCommittedOnce(replay, frames);
	EXPECT_EQ(replay.mfw_replays, 10u);
}

TEST(StreamReplayTest, CompressedEncodingFoldsALoneMidGroupZeroFrameIntoTheBurst) {
	auto part = absl::optional<xc7series::Part>(ReplayTestPart());
	const std::vector<uint32_t> fills = {
	    0xA1, 0,    0xA3, 0xA4, 0,    0,    0xA7, 0xA8, 0,    0xC2,
	    0xC3, 0xC4, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0};
	auto frames = AllFrames(*part, fills);
	auto blocks =
	    Configuration<Series7>::createCompressedFrameData(frames, part);
	ASSERT_EQ(blocks.size(), 4u);

	EXPECT_EQ(blocks[0].far, Far(xc7series::BlockType::CLB_IO_CLK, 0, 0, 0));
	EXPECT_TRUE(blocks[0].mfw);
	EXPECT_EQ(blocks[0].words.size(), 5u * 101);
	EXPECT_EQ(blocks[0].replay,
	          std::vector<uint32_t>(
	              {Far(xc7series::BlockType::CLB_IO_CLK, 0, 1, 1)}));

	EXPECT_FALSE(blocks[1].mfw);
	EXPECT_EQ(blocks[1].words.size(), 4u * 101);

	EXPECT_EQ(blocks[2].far, Far(xc7series::BlockType::CLB_IO_CLK, 1, 0, 0));
	EXPECT_FALSE(blocks[2].mfw);
	EXPECT_EQ(blocks[2].words.size(), 6u * 101);

	EXPECT_TRUE(blocks[3].mfw);
	EXPECT_EQ(blocks[3].words.size(), 8u * 101);
	EXPECT_TRUE(blocks[3].replay.empty());

	ASSERT_EQ(Configuration<Series7>::checkCompressedFrameSet(blocks,
	                                                          frames, part),
	          20u);
	FullStreamOptions options;
	options.compressed = &blocks;
	Series7::ConfigurationPackage packets;
	Configuration<Series7>::createConfigurationPackage(
	    packets, Configuration<Series7>::PacketData(), part, options);
	xc7series::StreamReplay replay;
	std::string error;
	ASSERT_TRUE(xc7series::ReplayConfigurationStream(
	    Serialize(packets), *part, &replay, &error))
	    << error;
	ExpectEachFrameCommittedOnce(replay, frames);
}

TEST(StreamReplayTest, CompressedStreamWithFarBeforeWcfgIsRefused) {
	auto part = absl::optional<xc7series::Part>(ReplayTestPart());
	auto frames = AllFrames(*part, kZeroRunFills);
	auto blocks =
	    Configuration<Series7>::createCompressedFrameData(frames, part);
	FullStreamOptions options;
	options.compressed = &blocks;
	Series7::ConfigurationPackage packets;
	Configuration<Series7>::createConfigurationPackage(
	    packets, Configuration<Series7>::PacketData(), part, options);

	using Packet = ConfigurationPacket<Series7::ConfRegType>;
	size_t swapped = 0;
	for (size_t i = 0; i + 1 < packets.size(); ++i) {
		const Packet& a = *packets[i];
		const Packet& b = *packets[i + 1];
		if (a.address() == Series7::ConfRegType::CMD &&
		    a.data().size() == 1 &&
		    a.data()[0] ==
		        static_cast<uint32_t>(xc7series::Command::WCFG) &&
		    b.address() == Series7::ConfRegType::FAR) {
			std::swap(packets[i], packets[i + 1]);
			++swapped;
			break;
		}
	}
	ASSERT_EQ(swapped, 1u);
	xc7series::StreamReplay replay;
	std::string error;
	EXPECT_FALSE(xc7series::ReplayConfigurationStream(
	    Serialize(packets), *part, &replay, &error));
	EXPECT_NE(error.find("not followed by an MFWR"), std::string::npos)
	    << error;
}

TEST(StreamReplayTest, BurstHoldsItsLastFrameBack) {
	auto part = ReplayTestPart();
	const uint32_t a0 = Far(xc7series::BlockType::CLB_IO_CLK, 0, 0, 0);
	const uint32_t a1 = Far(xc7series::BlockType::CLB_IO_CLK, 0, 0, 1);
	auto bytes = Stream()
	                 .Cmd(xc7series::Command::WCFG)
	                 .FarWrite(a0)
	                 .Fdri(Concat({Frame(1), Frame(2)}))
	                 .Cmd(xc7series::Command::DESYNC)
	                 .Bytes();
	xc7series::StreamReplay replay;
	std::string error;
	ASSERT_TRUE(
	    xc7series::ReplayConfigurationStream(bytes, part, &replay, &error))
	    << error;
	ASSERT_EQ(replay.committed.size(), 1u);
	EXPECT_EQ(replay.committed[0].address, a0);
	ASSERT_EQ(replay.discarded.size(), 1u);
	EXPECT_EQ(replay.discarded[0].address, a1);
	EXPECT_EQ(replay.discarded[0].words, Frame(2));
}

TEST(StreamReplayTest, RowEndPadsLandInDummyPositions) {
	auto part = ReplayTestPart();
	const uint32_t first = Far(xc7series::BlockType::CLB_IO_CLK, 0, 1, 2);
	const uint32_t last = Far(xc7series::BlockType::CLB_IO_CLK, 0, 1, 3);
	for (int pads = 0; pads <= 3; ++pads) {
		std::vector<std::vector<uint32_t>> f = {Frame(1), Frame(2)};
		for (int p = 0; p < pads; ++p)
			f.push_back(Frame(0));
		auto bytes = Stream()
		                 .Cmd(xc7series::Command::WCFG)
		                 .FarWrite(first)
		                 .Fdri(Concat(f))
		                 .Cmd(xc7series::Command::DESYNC)
		                 .Bytes();
		xc7series::StreamReplay replay;
		std::string error;
		ASSERT_TRUE(xc7series::ReplayConfigurationStream(
		    bytes, part, &replay, &error))
		    << error;
		if (pads == 0) {
			ASSERT_EQ(replay.committed.size(), 1u);
			ASSERT_EQ(replay.discarded.size(), 1u);
			EXPECT_EQ(replay.discarded[0].address, last);
		} else if (pads <= 2) {
			ASSERT_EQ(replay.committed.size(), 2u) << pads;
			EXPECT_EQ(replay.committed[1].address, last);
			EXPECT_TRUE(replay.discarded.empty());
			EXPECT_EQ(replay.dummy_frames,
			          static_cast<size_t>(pads));
		} else {
			ASSERT_EQ(replay.committed.size(), 2u);
			ASSERT_EQ(replay.discarded.size(), 1u);
			EXPECT_EQ(
			    replay.discarded[0].address,
			    Far(xc7series::BlockType::CLB_IO_CLK, 1, 0, 0));
		}
	}
}

TEST(StreamReplayTest, MfwrOutsideMfwIsRefused) {
	auto part = ReplayTestPart();
	auto bytes = Stream()
	                 .Cmd(xc7series::Command::WCFG)
	                 .FarWrite(0)
	                 .Fdri(Concat({Frame(1), Frame(0)}))
	                 .Write(10, std::vector<uint32_t>(8, 0))
	                 .Cmd(xc7series::Command::DESYNC)
	                 .Bytes();
	xc7series::StreamReplay replay;
	std::string error;
	EXPECT_FALSE(
	    xc7series::ReplayConfigurationStream(bytes, part, &replay, &error));
	EXPECT_NE(error.find("not MFW"), std::string::npos) << error;
}

TEST(StreamReplayTest, MfwrLengthsAreTheVendorGrammar) {
	auto part = ReplayTestPart();
	const uint32_t replay_at =
	    Far(xc7series::BlockType::CLB_IO_CLK, 0, 0, 3);
	for (int variant = 0; variant < 3; ++variant) {
		Stream s;
		s.Cmd(xc7series::Command::WCFG)
		    .FarWrite(0)
		    .Fdri(Concat({Frame(1), Frame(0)}))
		    .Cmd(xc7series::Command::MFW)
		    .Write(10, std::vector<uint32_t>(variant == 1 ? 4 : 8, 0))
		    .FarWrite(replay_at)
		    .Write(10, std::vector<uint32_t>(variant == 2 ? 8 : 4, 0))
		    .Cmd(xc7series::Command::WCFG)
		    .Cmd(xc7series::Command::DESYNC);
		xc7series::StreamReplay replay;
		std::string error;
		const bool ok = xc7series::ReplayConfigurationStream(
		    s.Bytes(), part, &replay, &error);
		if (variant == 0) {
			ASSERT_TRUE(ok) << error;
			ASSERT_EQ(replay.committed.size(), 3u);
			EXPECT_EQ(replay.committed[2].address, replay_at);
			EXPECT_EQ(replay.committed[2].words, Frame(0));
		} else {
			EXPECT_FALSE(ok) << variant;
		}
	}
}

TEST(StreamReplayTest, FdriOutsideWcfgIsRefused) {
	auto part = ReplayTestPart();
	auto bytes = Stream()
	                 .Cmd(xc7series::Command::RCRC)
	                 .FarWrite(0)
	                 .Fdri(Concat({Frame(1), Frame(0)}))
	                 .Cmd(xc7series::Command::DESYNC)
	                 .Bytes();
	xc7series::StreamReplay replay;
	std::string error;
	EXPECT_FALSE(
	    xc7series::ReplayConfigurationStream(bytes, part, &replay, &error));
}

TEST(StreamReplayTest, ForeignIdcodeIsRefused) {
	auto part = ReplayTestPart();
	xc7series::StreamReplay replay;
	std::string error;
	EXPECT_TRUE(xc7series::ReplayConfigurationStream(
	    Stream(false).Write(12, {0x1234}).Bytes(), part, &replay, &error))
	    << error;
	EXPECT_FALSE(xc7series::ReplayConfigurationStream(
	    Stream(false).Write(12, {0x4321}).Bytes(), part, &replay, &error));
}

TEST(StreamReplayTest, FdriBeforeAnyIdcodeWriteIsRefused) {
	auto part = ReplayTestPart();
	const uint32_t a0 = Far(xc7series::BlockType::CLB_IO_CLK, 0, 0, 0);
	auto build = [a0](bool idcode, bool idcode_after) {
		Stream s(idcode);
		s.Cmd(xc7series::Command::WCFG)
		    .FarWrite(a0)
		    .Fdri(Concat({Frame(1), Frame(2)}));
		if (idcode_after)
			s.Write(12, {kReplayTestIdcode});
		s.Cmd(xc7series::Command::DESYNC);
		return s.Bytes();
	};
	xc7series::StreamReplay replay;
	std::string error;
	EXPECT_TRUE(xc7series::ReplayConfigurationStream(build(true, false),
	                                                 part, &replay, &error))
	    << error;
	for (bool after : {false, true}) {
		error.clear();
		EXPECT_FALSE(xc7series::ReplayConfigurationStream(
		    build(false, after), part, &replay, &error))
		    << after;
		EXPECT_NE(error.find("before any IDCODE write"),
		          std::string::npos)
		    << error;
	}
}

TEST(StreamReplayTest, TruncatedPacketIsRefused) {
	auto part = ReplayTestPart();
	auto bytes = Stream()
	                 .Cmd(xc7series::Command::WCFG)
	                 .FarWrite(0)
	                 .Fdri(Concat({Frame(1), Frame(0)}))
	                 .Bytes();
	bytes.resize(bytes.size() - 8);
	xc7series::StreamReplay replay;
	std::string error;
	EXPECT_FALSE(
	    xc7series::ReplayConfigurationStream(bytes, part, &replay, &error));
}

TEST(StreamReplayTest, CompressedFrameSetCheckRefusesWrongReplays) {
	auto part = absl::optional<xc7series::Part>(ReplayTestPart());
	auto frames = AllFrames(*part, kZeroRunFills);
	const auto good =
	    Configuration<Series7>::createCompressedFrameData(frames, part);
	ASSERT_EQ(
	    Configuration<Series7>::checkCompressedFrameSet(good, frames, part),
	    20u);
	size_t with_replays = 0;
	while (with_replays < good.size() && good[with_replays].replay.empty())
		++with_replays;
	ASSERT_LT(with_replays, good.size());

	auto dropped = good;
	dropped[with_replays].replay.pop_back();
	EXPECT_THROW(Configuration<Series7>::checkCompressedFrameSet(
	                 dropped, frames, part),
	             std::runtime_error);

	auto duplicated = good;
	duplicated[with_replays].replay.push_back(
	    duplicated[with_replays].replay.back());
	EXPECT_THROW(Configuration<Series7>::checkCompressedFrameSet(
	                 duplicated, frames, part),
	             std::runtime_error);

	auto shifted = good;
	shifted[0].words.erase(shifted[0].words.end() - 101,
	                       shifted[0].words.end());
	EXPECT_THROW(Configuration<Series7>::checkCompressedFrameSet(
	                 shifted, frames, part),
	             std::runtime_error);
}

TEST(StreamReplayTest, FramesPastTheLastDevicePositionAreRefused) {
	auto part = ReplayTestPart();
	const uint32_t last = Far(xc7series::BlockType::BLOCK_RAM, 0, 1, 3);
	for (int pads = 0; pads <= 3; ++pads) {
		std::vector<std::vector<uint32_t>> f = {Frame(1)};
		for (int p = 0; p < pads; ++p)
			f.push_back(Frame(0));
		auto bytes = Stream()
		                 .Cmd(xc7series::Command::WCFG)
		                 .FarWrite(last)
		                 .Fdri(Concat(f))
		                 .Cmd(xc7series::Command::DESYNC)
		                 .Bytes();
		xc7series::StreamReplay replay;
		std::string error;
		const bool ok = xc7series::ReplayConfigurationStream(
		    bytes, part, &replay, &error);
		if (pads <= 2) {
			EXPECT_TRUE(ok) << pads << ": " << error;
		} else {
			EXPECT_FALSE(ok) << pads;
			EXPECT_NE(error.find("past the last frame"),
			          std::string::npos)
			    << error;
		}
	}
}

namespace {

struct PrePassCase {
	xc7series::ReplayFrameMap want;
	xc7series::ReplayFrameMap prepass;
	xc7series::ReplayFrameMap statics;
	xc7series::StreamReplay replay;
	uint32_t a = Far(xc7series::BlockType::CLB_IO_CLK, 0, 0, 0);
	uint32_t b = Far(xc7series::BlockType::CLB_IO_CLK, 0, 0, 1);
	uint32_t held = Far(xc7series::BlockType::CLB_IO_CLK, 0, 0, 2);

	PrePassCase() {
		want[xc7series::FrameAddress(a)] = Frame(0xA2);
		want[xc7series::FrameAddress(b)] = Frame(0xB2);
		prepass[xc7series::FrameAddress(a)] = Frame(0xA1);
		prepass[xc7series::FrameAddress(b)] = Frame(0xB1);
		statics[xc7series::FrameAddress(held)] = Frame(0x55);
		replay.committed = {{a, Frame(0xA1)},
		                    {b, Frame(0xB1)},
		                    {a, Frame(0xA2)},
		                    {b, Frame(0xB2)}};
		replay.discarded = {{held, Frame(0x55)}};
		replay.desynced = true;
	}

	bool Check(std::string* error) const {
		return xc7series::CheckReplayedFrames(replay, want, statics,
		                                      &prepass, error);
	}
};

}  // namespace

TEST(StreamReplayTest, ReplayCheckAcceptsAWellFormedPrePass) {
	PrePassCase c;
	std::string error;
	EXPECT_TRUE(c.Check(&error)) << error;
}

TEST(StreamReplayTest, ReplayCheckRefusesPassTwoEmittedTwice) {
	PrePassCase c;
	c.replay.committed[0].words = Frame(0xA2);
	c.replay.committed[1].words = Frame(0xB2);
	std::string error;
	EXPECT_FALSE(c.Check(&error));
	EXPECT_NE(error.find("committed first with words that differ"),
	          std::string::npos)
	    << error;
}

TEST(StreamReplayTest, ReplayCheckRefusesPassOneAfterPassTwo) {
	PrePassCase c;
	c.replay.committed = {{c.a, Frame(0xA1)},
	                      {c.a, Frame(0xA2)},
	                      {c.b, Frame(0xB1)},
	                      {c.b, Frame(0xB2)}};
	std::string error;
	EXPECT_FALSE(c.Check(&error));
	EXPECT_NE(error.find("pass 1 of every block must precede"),
	          std::string::npos)
	    << error;
}

TEST(StreamReplayTest, ReplayCheckRefusesAnAddressTheInputDidNotAskFor) {
	PrePassCase c;
	c.replay.committed.push_back(
	    {Far(xc7series::BlockType::CLB_IO_CLK, 0, 1, 0), Frame(0)});
	std::string error;
	EXPECT_FALSE(c.Check(&error));
	EXPECT_NE(error.find("the input did not ask for it"), std::string::npos)
	    << error;
}

TEST(StreamReplayTest, ReplayCheckRefusesAHeldBackFrameThatIsNotStatic) {
	PrePassCase c;
	c.replay.discarded[0].words = Frame(0x66);
	std::string error;
	EXPECT_FALSE(c.Check(&error));
	EXPECT_NE(error.find("does not carry the static's own content"),
	          std::string::npos)
	    << error;

	PrePassCase d;
	d.statics.clear();
	EXPECT_FALSE(d.Check(&error));
	EXPECT_NE(error.find("has no static frame"), std::string::npos)
	    << error;
}

TEST(StreamReplayTest, ReplayCheckRefusesWrongCountsAndPayloads) {
	std::string error;

	PrePassCase once;
	once.replay.committed.erase(once.replay.committed.begin());
	EXPECT_FALSE(once.Check(&error));
	EXPECT_NE(error.find("committed 1 time(s), expected 2"),
	          std::string::npos)
	    << error;

	PrePassCase last;
	last.replay.committed[3].words = Frame(0xBB);
	EXPECT_FALSE(last.Check(&error));
	EXPECT_NE(error.find("committed last with words that differ"),
	          std::string::npos)
	    << error;

	PrePassCase no_desync;
	no_desync.replay.desynced = false;
	EXPECT_FALSE(no_desync.Check(&error));
	EXPECT_NE(error.find("DESYNC"), std::string::npos) << error;

	PrePassCase full;
	full.replay.committed.erase(full.replay.committed.begin(),
	                            full.replay.committed.begin() + 2);
	EXPECT_TRUE(xc7series::CheckReplayedFrames(
	    full.replay, full.want, full.statics, nullptr, &error))
	    << error;
	full.replay.committed.push_back({full.a, Frame(0xA2)});
	EXPECT_FALSE(xc7series::CheckReplayedFrames(
	    full.replay, full.want, full.statics, nullptr, &error));
	EXPECT_NE(error.find("committed 2 time(s), expected 1"),
	          std::string::npos)
	    << error;
}

namespace {

std::vector<uint8_t> ReadFile(const char* path) {
	auto file = prjxray::MemoryMappedFile::InitWithFile(path);
	EXPECT_TRUE(file) << path;
	if (!file)
		return {};
	auto bytes = file->as_bytes();
	return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

std::map<uint32_t, std::vector<std::vector<uint32_t>>> ByAddress(
    const std::vector<xc7series::ReplayedFrame>& frames) {
	std::map<uint32_t, std::vector<std::vector<uint32_t>>> out;
	for (const auto& f : frames)
		out[f.address].push_back(f.words);
	return out;
}

}  // namespace

TEST(StreamReplayTest, PerFrameCrcStreamCommitsWhatThePlainStreamCommits) {
	auto part = xc7series::Part::FromFile("configuration_test.yaml");
	ASSERT_TRUE(part);

	xc7series::StreamReplay plain;
	std::string error;
	ASSERT_TRUE(xc7series::ReplayConfigurationStream(
	    ReadFile("configuration_test.bit"), *part, &plain, &error))
	    << error;
	EXPECT_TRUE(plain.discarded.empty());

	xc7series::StreamReplay per_frame;
	ASSERT_TRUE(xc7series::ReplayConfigurationStream(
	    ReadFile("configuration_test.perframecrc.bit"), *part, &per_frame,
	    &error))
	    << error;
	EXPECT_TRUE(per_frame.desynced);
	EXPECT_EQ(per_frame.dummy_frames, 0u);

	auto want = ByAddress(plain.committed);
	auto got = ByAddress(per_frame.committed);
	EXPECT_EQ(got.size(), want.size());
	size_t once = 0;
	for (const auto& w : want) {
		auto it = got.find(w.first);
		ASSERT_NE(it, got.end()) << std::hex << w.first;
		ASSERT_EQ(it->second.size(), 1u) << std::hex << w.first;
		EXPECT_EQ(it->second, w.second) << std::hex << w.first;
		++once;
	}
	EXPECT_EQ(once, 5408u);

	ASSERT_EQ(per_frame.discarded.size(), 6u);
	for (const auto& d : per_frame.discarded) {
		auto it = got.find(d.address);
		ASSERT_NE(it, got.end()) << std::hex << d.address;
		EXPECT_EQ(d.words, it->second[0]) << std::hex << d.address;
	}
	EXPECT_EQ(per_frame.discarded.back().address, 0x00c0017fu);
}

TEST(StreamReplayTest, PerFrameCrcGrammarRefusesAFrameWithNoFarMarker) {
	auto part = absl::optional<xc7series::Part>(ReplayTestPart());
	auto a0 = Far(xc7series::BlockType::CLB_IO_CLK, 0, 0, 0);
	auto a1 = Far(xc7series::BlockType::CLB_IO_CLK, 0, 0, 1);
	auto stream = [&](bool marker, bool two_frames,
	                  uint32_t mask = 0x00200000) {
		Stream s;
		s.Write(6, {mask}).Write(24, {0x00200000});
		s.Cmd(xc7series::Command::WCFG).FarWrite(a0);
		if (two_frames)
			s.Write(2, Concat({Frame(0xA0), Frame(0xA1)}));
		else
			s.Write(2, Frame(0xA0));
		s.FarWrite(a0).Write(0, {0});
		s.Write(2, Frame(0xA1));
		if (marker)
			s.FarWrite(a1);
		s.Write(0, {0}).Write(2, Frame(0xA1)).FarWrite(a1);
		return s.Bytes();
	};
	xc7series::StreamReplay replay;
	std::string error;
	ASSERT_TRUE(xc7series::ReplayConfigurationStream(stream(true, false),
	                                                 *part, &replay, &error))
	    << error;
	ASSERT_EQ(replay.committed.size(), 2u);
	EXPECT_EQ(replay.committed[0].address, a0);
	EXPECT_EQ(replay.committed[0].words, Frame(0xA0));
	EXPECT_EQ(replay.committed[1].address, a1);
	EXPECT_EQ(replay.committed[1].words, Frame(0xA1));

	EXPECT_FALSE(xc7series::ReplayConfigurationStream(
	    stream(false, false), *part, &replay, &error));
	EXPECT_NE(error.find("FAR marker"), std::string::npos) << error;
	EXPECT_FALSE(xc7series::ReplayConfigurationStream(
	    stream(true, true), *part, &replay, &error));
	EXPECT_NE(error.find("one frame"), std::string::npos) << error;

	ASSERT_TRUE(xc7series::ReplayConfigurationStream(
	    stream(true, false, 0xffdfffff), *part, &replay, &error))
	    << error;
	EXPECT_TRUE(replay.committed.empty());
}
