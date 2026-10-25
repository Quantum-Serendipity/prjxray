/*
 * Copyright (C) 2017-2020  The Project X-Ray Authors.
 *
 * Use of this source code is governed by a ISC-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/ISC
 *
 * SPDX-License-Identifier: ISC
 */
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>
#include <prjxray/xilinx/bitstream_reader.h>
#include <prjxray/xilinx/bitstream_writer.h>
#include <prjxray/xilinx/configuration_packet.h>
#include <prjxray/xilinx/configuration_packet_with_payload.h>
#include <prjxray/xilinx/configuration_register.h>

#include <prjxray/bit_ops.h>

using namespace prjxray::xilinx;

constexpr uint32_t kType1NOP = prjxray::bit_field_set<uint32_t>(0, 31, 29, 0x1);

constexpr uint32_t MakeType1(const int opcode,
                             const int address,
                             const int word_count) {
	return prjxray::bit_field_set<uint32_t>(
	    prjxray::bit_field_set<uint32_t>(
	        prjxray::bit_field_set<uint32_t>(
	            prjxray::bit_field_set<uint32_t>(0x0, 31, 29, 0x1), 28, 27,
	            opcode),
	        26, 13, address),
	    10, 0, word_count);
}

constexpr uint32_t MakeType2(const int opcode, const int word_count) {
	return prjxray::bit_field_set<uint32_t>(
	    prjxray::bit_field_set<uint32_t>(
	        prjxray::bit_field_set<uint32_t>(0x0, 31, 29, 0x2), 28, 27,
	        opcode),
	    26, 0, word_count);
}

void dump_packets(BitstreamWriter<Series7> writer, bool nl = true) {
	int i = 0;
	// for (uint32_t x : itr) {
	for (auto itr = writer.begin(); itr != writer.end(); ++itr) {
		if (nl) {
			printf("% 3d: 0x0%08X\n", i, *itr);
		} else {
			printf("0x0%08X, ", *itr);
		}
		fflush(stdout);
		++i;
	}
	if (!nl) {
		printf("\n");
	}
}

// Special all 0's
void AddType0(
    std::vector<std::unique_ptr<ConfigurationPacket<Series7::ConfRegType>>>&
        packets) {
	// InitWithWords doesn't like type 0
	/*
	static std::vector<uint32_t> words{0x00000000};
	absl::Span<uint32_t> word_span(words);
	auto packet =
	ConfigurationPacket<Series7::ConfRegType>::InitWithWords(word_span);
	packets.push_back(*(packet.second));
	*/
	static std::vector<uint32_t> words{};
	absl::Span<uint32_t> word_span(words);
	// CRC is config value 0
	packets.emplace_back(new ConfigurationPacket<Series7::ConfRegType>(
	    0, ConfigurationPacket<Series7::ConfRegType>::NOP,
	    Series7::ConfRegType::CRC, word_span));
}

void AddType1(
    std::vector<std::unique_ptr<ConfigurationPacket<Series7::ConfRegType>>>&
        packets) {
	static std::vector<uint32_t> words{MakeType1(0x2, 0x3, 2), 0xAA, 0xBB};
	absl::Span<uint32_t> word_span(words);
	auto packet =
	    ConfigurationPacket<Series7::ConfRegType>::InitWithWords(word_span);
	packets.emplace_back(
	    new ConfigurationPacket<Series7::ConfRegType>(*(packet.second)));
}

// Empty
void AddType1E(
    std::vector<std::unique_ptr<ConfigurationPacket<Series7::ConfRegType>>>&
        packets) {
	static std::vector<uint32_t> words{MakeType1(0x2, 0x3, 0)};
	absl::Span<uint32_t> word_span(words);
	auto packet =
	    ConfigurationPacket<Series7::ConfRegType>::InitWithWords(word_span);
	packets.emplace_back(
	    new ConfigurationPacket<Series7::ConfRegType>(*(packet.second)));
}

void AddType2(
    std::vector<std::unique_ptr<ConfigurationPacket<Series7::ConfRegType>>>&
        packets) {
	// Type 1 packet with address
	// Without this InitWithWords will fail on type 2
	ConfigurationPacket<Series7::ConfRegType>* packet1;
	{
		static std::vector<uint32_t> words{MakeType1(0x2, 0x3, 0)};
		absl::Span<uint32_t> word_span(words);
		auto packet1_pair =
		    ConfigurationPacket<Series7::ConfRegType>::InitWithWords(
		        word_span);
		packets.emplace_back(
		    new ConfigurationPacket<Series7::ConfRegType>(
		        *(packet1_pair.second)));
		packet1 = packets[0].get();
	}
	// Type 2 packet with data
	{
		static std::vector<uint32_t> words{
		    MakeType2(0x01, 12), 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
		absl::Span<uint32_t> word_span(words);
		auto packet =
		    ConfigurationPacket<Series7::ConfRegType>::InitWithWords(
		        word_span, packet1);
		packets.emplace_back(
		    new ConfigurationPacket<Series7::ConfRegType>(
		        *(packet.second)));
	}
}

// Empty packets should produce just the header
TEST(BitstreamWriterTest, WriteHeader) {
	std::vector<std::unique_ptr<ConfigurationPacket<Series7::ConfRegType>>>
	    packets;

	BitstreamWriter<Series7> writer(packets);
	std::vector<uint32_t> words(writer.begin(), writer.end());

	// Per UG470 pg 80: Bus Width Auto Detection
	std::vector<uint32_t> ref_header{
	    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
	    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x000000BB, 0x11220044,
	    0xFFFFFFFF, 0xFFFFFFFF, 0xAA995566};
	EXPECT_EQ(words, ref_header);

	// dump_packets(writer);
}

TEST(BitstreamWriterTest, WriteType0) {
	std::vector<std::unique_ptr<ConfigurationPacket<Series7::ConfRegType>>>
	    packets;
	AddType0(packets);
	BitstreamWriter<Series7> writer(packets);
	// dump_packets(writer, false);
	std::vector<uint32_t> words(writer.begin(), writer.end());
	std::vector<uint32_t> ref{
	    // Bus width + sync
	    0x0FFFFFFFF, 0x0FFFFFFFF, 0x0FFFFFFFF, 0x0FFFFFFFF, 0x0FFFFFFFF,
	    0x0FFFFFFFF, 0x0FFFFFFFF, 0x0FFFFFFFF, 0x0000000BB, 0x011220044,
	    0x0FFFFFFFF, 0x0FFFFFFFF, 0x0AA995566,
	    // Type 0
	    0x00000000};
	EXPECT_EQ(words, ref);
}
TEST(BitstreamWriterTest, WriteType1) {
	std::vector<std::unique_ptr<ConfigurationPacket<Series7::ConfRegType>>>
	    packets;
	AddType1(packets);
	BitstreamWriter<Series7> writer(packets);
	// dump_packets(writer, false);
	std::vector<uint32_t> words(writer.begin(), writer.end());
	std::vector<uint32_t> ref{
	    // Bus width + sync
	    0x0FFFFFFFF, 0x0FFFFFFFF, 0x0FFFFFFFF, 0x0FFFFFFFF, 0x0FFFFFFFF,
	    0x0FFFFFFFF, 0x0FFFFFFFF, 0x0FFFFFFFF, 0x0000000BB, 0x011220044,
	    0x0FFFFFFFF, 0x0FFFFFFFF, 0x0AA995566,
	    // Type 1
	    0x030006002, 0x0000000AA, 0x0000000BB};
	EXPECT_EQ(words, ref);
}

TEST(BitstreamWriterTest, WriteType2) {
	std::vector<std::unique_ptr<ConfigurationPacket<Series7::ConfRegType>>>
	    packets;
	AddType2(packets);
	BitstreamWriter<Series7> writer(packets);
	// dump_packets(writer, false);
	std::vector<uint32_t> words(writer.begin(), writer.end());
	std::vector<uint32_t> ref{
	    // Bus width + sync
	    0x0FFFFFFFF, 0x0FFFFFFFF, 0x0FFFFFFFF, 0x0FFFFFFFF, 0x0FFFFFFFF,
	    0x0FFFFFFFF, 0x0FFFFFFFF, 0x0FFFFFFFF, 0x0000000BB, 0x011220044,
	    0x0FFFFFFFF, 0x0FFFFFFFF, 0x0AA995566,
	    // Type 1 + type 2 header
	    0x030006000, 0x04800000C, 0x000000001, 0x000000002, 0x000000003,
	    0x000000004, 0x000000005, 0x000000006, 0x000000007, 0x000000008,
	    0x000000009, 0x00000000A, 0x00000000B, 0x00000000C};
	EXPECT_EQ(words, ref);
}

TEST(BitstreamWriterTest, WriteMulti) {
	std::vector<std::unique_ptr<ConfigurationPacket<Series7::ConfRegType>>>
	    packets;
	AddType1(packets);
	AddType1E(packets);
	AddType2(packets);
	AddType1E(packets);
	BitstreamWriter<Series7> writer(packets);
	// dump_packets(writer, false);
	std::vector<uint32_t> words(writer.begin(), writer.end());
	std::vector<uint32_t> ref{
	    // Bus width + sync
	    0x0FFFFFFFF, 0x0FFFFFFFF, 0x0FFFFFFFF, 0x0FFFFFFFF, 0x0FFFFFFFF,
	    0x0FFFFFFFF, 0x0FFFFFFFF, 0x0FFFFFFFF, 0x0000000BB, 0x011220044,
	    0x0FFFFFFFF, 0x0FFFFFFFF, 0x0AA995566,
	    // Type1
	    0x030006002, 0x0000000AA, 0x0000000BB,
	    // Type1
	    0x030006000,
	    // Type 1 + type 2 header
	    0x030006000, 0x04800000C, 0x000000001, 0x000000002, 0x000000003,
	    0x000000004, 0x000000005, 0x000000006, 0x000000007, 0x000000008,
	    0x000000009, 0x00000000A, 0x00000000B, 0x00000000C,
	    // Type 1
	    0x030006000};
	EXPECT_EQ(words, ref);
}

namespace {

std::vector<uint8_t> WriteWithEpoch(const char* epoch,
                                    const std::string& frames_file,
                                    const std::string& part_name,
                                    int* rc) {
	if (epoch)
		setenv("SOURCE_DATE_EPOCH", epoch, 1);
	else
		unsetenv("SOURCE_DATE_EPOCH");
	Series7::ConfigurationPackage packets;
	packets.emplace_back(new ConfigurationPacketWithPayload<1, Series7::ConfRegType>(
	    ConfigurationPacket<Series7::ConfRegType>::Opcode::Write,
	    Series7::ConfRegType::IDCODE, {0x0362c093}));
	const std::string path = testing::TempDir() + "header_test.bit";
	std::remove(path.c_str());
	BitstreamWriter<Series7> writer(packets);
	*rc = writer.writeBitstream(packets, part_name, frames_file,
	                            "xc7frames2bit", path);
	unsetenv("SOURCE_DATE_EPOCH");
	std::ifstream in(path, std::ios::binary);
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
	                           std::istreambuf_iterator<char>());
	std::remove(path.c_str());
	return bytes;
}

std::string HeaderText(const std::vector<uint8_t>& bytes) {
	std::string out;
	for (uint8_t b : bytes)
		out.push_back(b >= 0x20 && b < 0x7f ? static_cast<char>(b) : '|');
	return out;
}

}  // namespace

TEST(BitstreamWriterTest, HeaderIsReproducibleUnderSourceDateEpoch) {
	int rc_a = -1, rc_b = -1;
	auto a = WriteWithEpoch("0", "/abs/dir/design.frames", "xc7a100tfgg484",
	                        &rc_a);
	auto b = WriteWithEpoch("0", "rel/other-dir/design.frames",
	                        "xc7a100tfgg484", &rc_b);
	ASSERT_EQ(rc_a, 0);
	ASSERT_EQ(rc_b, 0);
	EXPECT_EQ(a, b);
	const std::string text = HeaderText(a);
	EXPECT_NE(text.find("design.frames;Generator=xc7frames2bit"),
	          std::string::npos)
	    << text.substr(0, 120);
	EXPECT_EQ(text.find("/abs/dir"), std::string::npos);
	EXPECT_NE(text.find("1970/01/01"), std::string::npos);
	EXPECT_NE(text.find("00:00:00"), std::string::npos);

	auto c = WriteWithEpoch("1792801234", "design.frames", "xc7a100tfgg484",
	                        &rc_a);
	ASSERT_EQ(rc_a, 0);
	EXPECT_NE(HeaderText(c).find("2026/10/24"), std::string::npos);
	EXPECT_NE(HeaderText(c).find("00:20:34"), std::string::npos);
}

TEST(BitstreamWriterTest, HeaderRefusesABadEpochAndAnOverlongField) {
	int rc = 0;
	auto bad = WriteWithEpoch("-5", "design.frames", "xc7a100tfgg484", &rc);
	EXPECT_EQ(rc, 1);
	EXPECT_TRUE(bad.empty());
	WriteWithEpoch("12abc", "design.frames", "xc7a100tfgg484", &rc);
	EXPECT_EQ(rc, 1);
	WriteWithEpoch("", "design.frames", "xc7a100tfgg484", &rc);
	EXPECT_EQ(rc, 1);

	WriteWithEpoch("0", "design.frames", std::string(65534, 'p'), &rc);
	EXPECT_EQ(rc, 0);
	auto overlong = WriteWithEpoch("0", "design.frames",
	                               std::string(65535, 'p'), &rc);
	EXPECT_EQ(rc, 1);
	EXPECT_TRUE(overlong.empty());
}
