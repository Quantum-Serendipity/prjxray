/*
 * Copyright (C) 2017-2020  The Project X-Ray Authors.
 *
 * Use of this source code is governed by a ISC-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/ISC
 *
 * SPDX-License-Identifier: ISC
 */
#include <fstream>
#include <iostream>

#include <absl/strings/str_cat.h>
#include <absl/strings/str_split.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include <prjxray/xilinx/architectures.h>
#include <prjxray/xilinx/bitstream_writer.h>
#include <prjxray/xilinx/configuration.h>
#include <prjxray/xilinx/configuration_packet_with_payload.h>
#include <prjxray/xilinx/nop_packet.h>
#include <prjxray/xilinx/spartan6/command.h>
#include <prjxray/xilinx/xc7series/command.h>
#include <prjxray/xilinx/xc7series/configuration_options_0_value.h>
#include <prjxray/xilinx/xc7series/crc.h>
#include <prjxray/xilinx/xc7series/ecc.h>

namespace prjxray {
namespace xilinx {

template <>
Configuration<Spartan6>::PacketData
Configuration<Spartan6>::createType2ConfigurationPacketData(
    const Frames<Spartan6>::Frames2Data& frames,
    absl::optional<Spartan6::Part>& part) {
	// Generate a single type 2 packet that writes everything at once.
	PacketData packet_data;
	for (auto& frame : frames) {
		std::copy(frame.second.begin(), frame.second.end(),
		          std::back_inserter(packet_data));
	}

	// Insert payload length
	size_t packet_data_size = packet_data.size() - 2;
	packet_data.insert(packet_data.begin(), packet_data_size & 0xFFFF);
	packet_data.insert(packet_data.begin(),
	                   (packet_data_size >> 16) & 0xFFFF);
	return packet_data;
}

template <>
void Configuration<Spartan6>::createPartialConfigurationPackage(
    Spartan6::ConfigurationPackage& out_packets,
    const PartialPacketData& cfg_packets,
    absl::optional<Spartan6::Part>& part,
    const PartialStreamOptions& options) {
	throw std::runtime_error(
	    "Partial bitstream generation for this architecture is not "
	    "supported!");
}

template <>
void Configuration<Spartan6>::createConfigurationPackage(
    Spartan6::ConfigurationPackage& out_packets,
    const PacketData& packet_data,
    absl::optional<Spartan6::Part>& part) {
	using ArchType = Spartan6;
	using ConfigurationRegister = ArchType::ConfRegType;
	// Initialization sequence
	//
	// Reset CRC
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(spartan6::Command::RCRC)}));

	// NOP
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());

	// Frame length
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::FLR, {0x0380}));

	// Configuration Options 1
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::COR1, {0x3d08}));

	// Configurations Options2
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::COR2, {0x9ee}));

	// IDCODE
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<2, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::IDCODE,
	        {part->idcode() >> 16, part->idcode()}));

	// Control MASK
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::MASK, {0xcf}));

	// Control options
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CTL, {0x81}));

	// NOP packets
	for (int i = 0; i < 17; i++) {
		out_packets.emplace_back(
		    new NopPacket<ConfigurationRegister>());
	}

	// CCLK FREQ
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CCLK_FREQ, {0x3cc8}));

	// PWRDN_REG
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::PWRDN_REG, {0x881}));

	// EYE MASK
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::EYE_MASK, {0x0}));

	// House Clean Option
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::HC_OPT_REG, {0x1f}));

	// Configuration Watchdog Timer
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CWDT, {0xffff}));

	// GWE cycle during wake-up from suspend
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::PU_GWE, {0x5}));

	// GTS cycle during wake-up from suspend
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::PU_GTS, {0x4}));

	// Reboot mode
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::MODE_REG, {0x100}));

	// General options 1
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::GENERAL1, {0x0}));

	// General options 2
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::GENERAL2, {0x0}));

	// General options 3
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::GENERAL3, {0x0}));

	// General options 4
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::GENERAL4, {0x0}));

	// General options 5
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::GENERAL5, {0x0}));

	// SEU frequency, enable and status
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::SEU_OPT, {0x1be2}));

	// Expected readback signature for SEU detection
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<2, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::EXP_SIGN, {0x0, 0x0}));

	// NOP
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());

	// FAR
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<2, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::FAR_MAJ, {0x0, 0x0}));

	// Write Configuration Data
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(spartan6::Command::WCFG)}));

	// Frame data write
	out_packets.emplace_back(new ConfigurationPacket<ConfigurationRegister>(
	    TYPE2, ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	    ConfigurationRegister::FDRI, {packet_data}));

	// NOP packets
	for (int i = 0; i < 24; i++) {
		out_packets.emplace_back(
		    new NopPacket<ConfigurationRegister>());
	}

	// Finalization sequence
	//
	// Set/reset the IOB and CLB flip-flops
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(spartan6::Command::GRESTORE)}));

	// Last Frame
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(spartan6::Command::LFRM)}));

	// NOP packets
	for (int i = 0; i < 4; i++) {
		out_packets.emplace_back(
		    new NopPacket<ConfigurationRegister>());
	}

	// Set/reset the IOB and CLB flip-flops
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(spartan6::Command::GRESTORE)}));

	// Startup sequence
	//
	// Start
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(spartan6::Command::START)}));

	// Control MASK
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::MASK, {0xff}));

	// Control options
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CTL, {0x81}));

	// CRC
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<2, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CRC, {0x39, 0xe423}));

	// Desync
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(spartan6::Command::DESYNC)}));

	// NOP packets
	for (int i = 0; i < 14; i++) {
		out_packets.emplace_back(
		    new NopPacket<ConfigurationRegister>());
	}
}

// ---------------------------------------------------------------- pre-pass --
//
// docs/far-double-write-verdict.md section 5.1. See the declaration in
// configuration.h for the rule and for why the block-type restriction below is
// there even though section 5.1 does not state one.
namespace {

constexpr size_t kEccWord = 0x32;

// The bit positions a SITE tile owns -- what pass 1 keeps of the RM.
uint32_t SiteMask(uint8_t minor, size_t word) {
	if (word == kEccWord)
		return 0;  // word 50 belongs to the HCLK tile alone
	if (minor == 0)
		return 0x11111111;  // CLB owns bit index == 0 (mod 4)
	if (minor == 1)
		return 0x88888888;  // CLB owns bit index == 3 (mod 4)
	if (minor >= 26 && minor <= 35)
		return 0xFFFFFFFF;  // the CLB planes, in full
	return 0;               // minors 2-25 are interconnect
}

// The fixed selector code pass 1 writes over the routing-mux planes. prjxray's
// INT_[LR] site-input muxes are five-bit codes spanning minors 21-25 at a
// single bit index, so this is ONE park of the selector group, not "blank
// 16-23 and force 24-25 high" -- those read as two effects and are one.
uint32_t ParkMask(uint8_t minor, size_t word) {
	if (minor >= 24 && minor <= 25 && word != kEccWord)
		return 0xFFFFFFFF;
	return 0;
}

}  // namespace

template <>
Frames<Series7>::Frames2Data Configuration<Series7>::createPrePassFrames(
    const Frames<Series7>::Frames2Data& pass2,
    const Frames<Series7>::Frames2Data& static_frames,
    bool have_static) {
	Frames<Series7>::Frames2Data pass1;
	const std::vector<uint32_t> kZeroFrame(Series7::words_per_frame, 0);

	for (const auto& frame : pass2) {
		const xc7series::FrameAddress address = frame.first;
		const auto& words = frame.second;

		// Every other block type is carried through unchanged. The
		// recipe is measured on CLB_IO_CLK frames; PARK applied to a
		// BLOCK_RAM frame at minor 24/25 would force it to all-ones,
		// and the golden's BRAM pass 1 is zero.
		if (address.block_type() != xc7series::BlockType::CLB_IO_CLK ||
		    words.size() != Series7::words_per_frame) {
			pass1.emplace(address, words);
			continue;
		}

		const auto it = static_frames.find(address);
		const std::vector<uint32_t>& stat =
		    (have_static && it != static_frames.end()) ? it->second
		                                              : kZeroFrame;
		const bool stat_ok = stat.size() == Series7::words_per_frame;

		std::vector<uint32_t> out(Series7::words_per_frame, 0);
		const uint8_t minor = address.minor();
		for (size_t w = 0; w < Series7::words_per_frame; ++w) {
			// Word 50 is 0 plus the computed ECC, at every minor
			// and whatever the static holds there. Section 5.1's
			// expression reads `STATIC[w] | ...`, which at w == 50
			// would carry the static's HCLK-tile bits (indices
			// 14-31) and its stale ECC straight through; its own
			// table row says word 50 -> "0 + computed ECC", and the
			// table is what the goldens show. Measured: across all
			// four modules, pass 1 has ZERO frames with word-50
			// bits 31:13 set, while pass 2 has 3, 3, 3 and 15. So
			// the withholding is unconditional and the expression,
			// read literally, is wrong here.
			if (w == kEccWord)
				continue;  // out[w] is already 0
			const uint32_t site = SiteMask(minor, w);
			const uint32_t park = ParkMask(minor, w);
			const uint32_t s = stat_ok ? stat[w] : 0;
			out[w] = (words[w] & site) | park |
			         (s & ~site & ~park);
		}
		// RECOMPUTE, do not copy. Vivado's pass 1 carries a correct
		// ECC in 825 of 825 frames in all four measured modules; the
		// all-ones frames at minors 24/25 look ECC-less only because
		// the correct ECC of that particular frame happens to be 0.
		xc7series::updateECC(out);
		pass1.emplace(address, std::move(out));
	}
	return pass1;
}

// --------------------------------------------------------------- stream CRC --

template <>
uint32_t Configuration<Series7>::computeStreamCrc(
    const Series7::ConfigurationPackage& packets) {
	using ConfigurationRegister = Series7::ConfRegType;
	uint32_t crc = 0;
	for (const auto& packet : packets) {
		if (packet->opcode() !=
		    ConfigurationPacket<ConfigurationRegister>::Opcode::Write)
			continue;
		const ConfigurationRegister reg = packet->address();
		// A write to the CRC register resets the accumulator (2/2 on
		// the full bitstream's two CRC writes, 1/2 without).
		if (reg == ConfigurationRegister::CRC) {
			crc = 0;
			continue;
		}
		// CMD:RCRC accumulates its own write and THEN resets, which is
		// the same final value as "reset and consume the write" and is
		// NOT the same as resetting first: 6/6 vendor values this way,
		// 1/6 the other.
		if (reg == ConfigurationRegister::CMD &&
		    packet->data().size() == 1 &&
		    packet->data()[0] ==
		        static_cast<uint32_t>(xc7series::Command::RCRC)) {
			crc = 0;
			continue;
		}
		for (uint32_t word : packet->data())
			crc = xc7series::icap_crc(static_cast<uint32_t>(reg),
			                          word, crc);
	}
	return crc;
}

template <>
void Configuration<Series7>::createPartialConfigurationPackage(
    Series7::ConfigurationPackage& out_packets,
    const PartialPacketData& cfg_packets,
    absl::optional<Series7::Part>& part,
    const PartialStreamOptions& options) {
	using ArchType = Series7;
	using ConfigurationRegister = ArchType::ConfRegType;
	auto write1 = [&out_packets](ConfigurationRegister reg, uint32_t value) {
		out_packets.emplace_back(
		    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
		        ConfigurationPacket<ConfigurationRegister>::Opcode::
		            Write,
		        reg, {value}));
	};
	auto nop = [&out_packets]() {
		out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	};

	// The word-for-word target is Vivado 2025.2's own stream; see
	// docs/partial-bitstream.md for the map this was written from, and
	// scripts/u62-packet-map.py to reproduce it.
	nop();
	write1(ConfigurationRegister::CMD,
	       static_cast<uint32_t>(xc7series::Command::RCRC));
	nop();
	nop();

	// A .bit loaded over JTAG carries IDCODE; a raw ICAP stream does not.
	if (options.emit_idcode)
		write1(ConfigurationRegister::IDCODE, part->idcode());

	write1(ConfigurationRegister::CMD,
	       static_cast<uint32_t>(xc7series::Command::NOP));
	// MASK/CTL0 in PR #1492's exact triple, which is Vivado's: two pairs
	// here (0x100/0x100 asserting GLUTMASK_B, then 0x400/0x400) and one
	// pair in the epilogue. The prior corpus said these were "typically
	// omitted"; the golden refutes that.
	write1(ConfigurationRegister::MASK, 0x100);
	write1(ConfigurationRegister::CTL0, 0x100);
	write1(ConfigurationRegister::MASK, 0x400);
	write1(ConfigurationRegister::CTL0, 0x400);

	// One WCFG + FAR + FDRI block per run of consecutive frames.
	// cfg_packets is a const reference into the caller's storage and the
	// spans below point straight at it: it MUST outlive out_packets.
	//
	// WCFG IS PER BLOCK. Per UG470 the command in CMD is re-executed on
	// every FAR write, so a single WCFG up front would also work on the
	// device -- but Vivado writes one per block and this stream is meant to
	// match it. That was PR #1492's one real divergence.
	//
	// To emit the FAR double write, the caller passes pass 1's blocks
	// followed by pass 2's. The order that produces is A B A B -- pass 1 of
	// every block, then pass 2 of every block -- which is Vivado's, and is
	// NOT the A A B B a "write each block twice" reading suggests.
	for (auto& block : cfg_packets) {
		write1(ConfigurationRegister::CMD,
		       static_cast<uint32_t>(xc7series::Command::WCFG));
		nop();
		write1(ConfigurationRegister::FAR, block.first);
		nop();
		// The standard type1-then-type2 idiom for a payload over 2047
		// words: a Type-1 FDRI write of length ZERO selects the
		// register, and the Type-2 packet that follows inherits it and
		// carries the words.
		out_packets.emplace_back(new ConfigurationPacket<
		                         ConfigurationRegister>(
		    TYPE1,
		    ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
		    ConfigurationRegister::FDRI, {}));
		out_packets.emplace_back(new ConfigurationPacket<
		                         ConfigurationRegister>(
		    TYPE2,
		    ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
		    ConfigurationRegister::FDRI, block.second));
	}

	// Finalization sequence. GLUTMASK_B deasserted.
	write1(ConfigurationRegister::MASK, 0x100);
	write1(ConfigurationRegister::CTL0, 0x0);
	// DO NOT "fix" this address. 0x03be0000 decodes to block_type = 7
	// (FAR bits 25:23), which is RESERVED on every 7-series part, so this
	// parks the frame-address register where no frame lives. It was
	// reported as a magic-number defect; it is not one. Vivado emits the
	// identical word in the identical position of its own partial
	// bitstreams: docs/far-double-write-verdict.md section 2 decodes it
	// straight out of a Vivado 2025.2 partial for xc7a100tcsg324-1 as
	//     FAR    =  0x03be0000  (blk7 tb0 row31 col0 minor0)
	write1(ConfigurationRegister::FAR, 0x3be0000);

	// The CRC write, or the two-word substitution that replaces it.
	// EVERYTHING accumulated is already in out_packets at this point and
	// nothing after this line is accumulated, so the CRC is computed over
	// exactly the words that precede it -- which is what the register
	// means.
	if (options.compute_crc) {
		write1(ConfigurationRegister::CRC,
		       Configuration<Series7>::computeStreamCrc(out_packets));
	} else {
		write1(ConfigurationRegister::CMD,
		       static_cast<uint32_t>(xc7series::Command::RCRC));
	}

	write1(ConfigurationRegister::CMD,
	       static_cast<uint32_t>(xc7series::Command::DESYNC));
	for (unsigned int ii = 0; ii < options.trailing_nops; ++ii)
		nop();
}

template <>
void Configuration<Series7>::createConfigurationPackage(
    Series7::ConfigurationPackage& out_packets,
    const PacketData& packet_data,
    absl::optional<Series7::Part>& part) {
	using ArchType = Series7;
	using ConfigurationRegister = ArchType::ConfRegType;
	// Initialization sequence
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::TIMER, {0x0}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::WBSTAR, {0x0}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::NOP)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::RCRC)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::UNKNOWN, {0x0}));

	// Configuration Options 0
	out_packets.emplace_back(new ConfigurationPacketWithPayload<
	                         1, ConfigurationRegister>(
	    ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	    ConfigurationRegister::COR0,
	    {xc7series::ConfigurationOptions0Value()
	         .SetAddPipelineStageForDoneIn(true)
	         .SetReleaseDonePinAtStartupCycle(
	             xc7series::ConfigurationOptions0Value::SignalReleaseCycle::
	                 Phase4)
	         .SetStallAtStartupCycleUntilDciMatch(
	             xc7series::ConfigurationOptions0Value::StallCycle::NoWait)
	         .SetStallAtStartupCycleUntilMmcmLock(
	             xc7series::ConfigurationOptions0Value::StallCycle::NoWait)
	         .SetReleaseGtsSignalAtStartupCycle(
	             xc7series::ConfigurationOptions0Value::SignalReleaseCycle::
	                 Phase5)
	         .SetReleaseGweSignalAtStartupCycle(
	             xc7series::ConfigurationOptions0Value::SignalReleaseCycle::
	                 Phase6)}));

	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::COR1, {0x0}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::IDCODE, {part->idcode()}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::SWITCH)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::MASK, {0x401}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CTL0, {0x501}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::MASK, {0x0}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CTL1, {0x0}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::FAR, {0x0}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::WCFG)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());

	// Frame data write
	out_packets.emplace_back(new ConfigurationPacket<ConfigurationRegister>(
	    TYPE1, ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	    ConfigurationRegister::FDRI, {}));
	out_packets.emplace_back(new ConfigurationPacket<ConfigurationRegister>(
	    TYPE2, ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	    ConfigurationRegister::FDRI, packet_data));

	// Finalization sequence
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::RCRC)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::GRESTORE)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::LFRM)}));
	for (int ii = 0; ii < 100; ++ii) {
		out_packets.emplace_back(
		    new NopPacket<ConfigurationRegister>());
	}
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::START)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::FAR, {0x3be0000}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::MASK, {0x501}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CTL0, {0x501}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::RCRC)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::DESYNC)}));
	for (int ii = 0; ii < 400; ++ii) {
		out_packets.emplace_back(
		    new NopPacket<ConfigurationRegister>());
	}
}

template <>
void Configuration<UltraScale>::createPartialConfigurationPackage(
    UltraScale::ConfigurationPackage& out_packets,
    const PartialPacketData& cfg_packets,
    absl::optional<UltraScale::Part>& part,
    const PartialStreamOptions& options) {
	throw std::runtime_error(
	    "Partial bitstream generation for this architecture is not "
	    "supported!");
}

template <>
void Configuration<UltraScale>::createConfigurationPackage(
    UltraScale::ConfigurationPackage& out_packets,
    const PacketData& packet_data,
    absl::optional<UltraScale::Part>& part) {
	using ArchType = UltraScale;
	using ConfigurationRegister = ArchType::ConfRegType;
	// Initialization sequence
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::TIMER, {0x0}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::WBSTAR, {0x0}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::NOP)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::RCRC)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::FAR, {0x0}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::UNKNOWN, {0x0}));

	// Configuration Options 0
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::COR0, {0x38003fe5}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::COR1, {0x400000}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::IDCODE, {part->idcode()}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::SWITCH)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::MASK, {0x1}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CTL0, {0x101}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::MASK, {0x0}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CTL1, {0x0}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::FAR, {0x0}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::WCFG)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());

	// Frame data write
	out_packets.emplace_back(new ConfigurationPacket<ConfigurationRegister>(
	    TYPE1, ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	    ConfigurationRegister::FDRI, {}));
	out_packets.emplace_back(new ConfigurationPacket<ConfigurationRegister>(
	    TYPE2, ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	    ConfigurationRegister::FDRI, packet_data));

	// Finalization sequence
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::RCRC)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::GRESTORE)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::LFRM)}));
	for (int ii = 0; ii < 100; ++ii) {
		out_packets.emplace_back(
		    new NopPacket<ConfigurationRegister>());
	}
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::START)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::FAR, {0x3be0000}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::MASK, {0x101}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CTL0, {0x101}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::RCRC)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::DESYNC)}));
	for (int ii = 0; ii < 400; ++ii) {
		out_packets.emplace_back(
		    new NopPacket<ConfigurationRegister>());
	}
}

template <>
void Configuration<UltraScalePlus>::createPartialConfigurationPackage(
    UltraScalePlus::ConfigurationPackage& out_packets,
    const PartialPacketData& cfg_packets,
    absl::optional<UltraScalePlus::Part>& part,
    const PartialStreamOptions& options) {
	throw std::runtime_error(
	    "Partial bitstream generation for this architecture is not "
	    "supported!");
}

template <>
void Configuration<UltraScalePlus>::createConfigurationPackage(
    UltraScalePlus::ConfigurationPackage& out_packets,
    const PacketData& packet_data,
    absl::optional<UltraScalePlus::Part>& part) {
	using ArchType = UltraScalePlus;
	using ConfigurationRegister = ArchType::ConfRegType;
	// Initialization sequence
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::TIMER, {0x0}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::WBSTAR, {0x0}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::NOP)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::RCRC)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::FAR, {0x0}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::UNKNOWN, {0x0}));

	// Configuration Options 0
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::COR0, {0x38003fe5}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::COR1, {0x400000}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::IDCODE, {part->idcode()}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::SWITCH)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::MASK, {0x1}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CTL0, {0x101}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::MASK, {0x0}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CTL1, {0x0}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::FAR, {0x0}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::WCFG)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());

	// Frame data write
	out_packets.emplace_back(new ConfigurationPacket<ConfigurationRegister>(
	    TYPE1, ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	    ConfigurationRegister::FDRI, {}));
	out_packets.emplace_back(new ConfigurationPacket<ConfigurationRegister>(
	    TYPE2, ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	    ConfigurationRegister::FDRI, packet_data));

	// Finalization sequence
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::RCRC)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::GRESTORE)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::LFRM)}));
	for (int ii = 0; ii < 100; ++ii) {
		out_packets.emplace_back(
		    new NopPacket<ConfigurationRegister>());
	}
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::START)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::FAR, {0x3be0000}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::MASK, {0x101}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CTL0, {0x101}));
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::RCRC)}));
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(new NopPacket<ConfigurationRegister>());
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::CMD,
	        {static_cast<uint32_t>(xc7series::Command::DESYNC)}));
	for (int ii = 0; ii < 400; ++ii) {
		out_packets.emplace_back(
		    new NopPacket<ConfigurationRegister>());
	}
}
}  // namespace xilinx
}  // namespace prjxray
