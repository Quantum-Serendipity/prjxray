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
#include <iomanip>
#include <iostream>
#include <sstream>

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
    absl::optional<Spartan6::Part>& part,
    const FullStreamOptions& options) {
	// COR1 IS THE ONE FULL-BITSTREAM REGISTER THIS WRITER OFFERS A KNOB ON,
	// AND THE KNOB IS SERIES7-ONLY. Spartan6 numbers COR1 0x0a,
	// not 0x0e, and UG380 gives it an unrelated layout with no
	// readback-CRC field at all.
	if (options.compressed) {
		throw std::runtime_error(
		    "FullStreamOptions::compressed is the multiple-frame-write "
		    "encoding, and its grammar was read out of Vivado's own "
		    "7-series compressed bitstreams. No other architecture here "
		    "has been measured, and an MFWR sequence guessed from this "
		    "one would emit at exit 0.");
	}

	if (options.cor1) {
		throw std::runtime_error(
		    "FullStreamOptions::cor1 is characterised for Series7 "
		    "only. Spartan6's COR1 is register 0x0a with a UG380 "
		    "layout that has no readback-CRC field. "
		    "There is no degraded version to fall back to: a 7-series "
		    "COR1 field pattern written into this register would be a "
		    "guess that emits at exit 0.");
	}

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
// configuration.h for the rule; SITE itself lives in PrePassGeometry, because
// it is a function of the tile lattice and not of (minor, word).
namespace {

constexpr size_t kEccWord = 0x32;

// The fixed selector code pass 1 writes over the routing-mux planes. prjxray's
// INT_[LR] site-input muxes are five-bit codes spanning minors 21-25 at a
// single bit index, so this is ONE park of the selector group, not "blank
// 16-23 and force 24-25 high" -- those read as two effects and are one.
//
// PARK is per (column, minor) ACTUALLY PRESENT in the block, which is what
// iterating the caller's frames gives for free. A FAR burst can stop
// mid-column: an ROI whose last column contributes only minor 0 never gets a
// park, and a writer that counted frames instead would force the wrong two
// frames high.
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
    bool have_static,
    const xc7series::PrePassGeometry& geometry,
    PrePassReport* report) {
	Frames<Series7>::Frames2Data pass1;
	PrePassReport sink;
	PrePassReport& rep = report ? *report : sink;
	const std::vector<uint32_t> kZeroFrame(Series7::words_per_frame, 0);

	for (const auto& frame : pass2) {
		const xc7series::FrameAddress address = frame.first;
		const auto& words = frame.second;

		// BLOCK TYPE FIRST. The transformation applies to block type 0
		// and to nothing else; every other block type is written twice
		// VERBATIM -- same payload, ECC word included, no mask, no
		// park, no recomputation. Two separate hazards live here.
		// PARK at a BLOCK_RAM minor 24/25 forces a whole frame of live
		// block RAM to 0xFFFFFFFF. And updateECC on a BLOCK_RAM frame
		// invents a frame ECC Vivado does not write: word 50 is zero in
		// 772 of 772 there, both passes, content or no content, and the
		// 16 frames per module it would corrupt are exactly the ones
		// carrying data. Verdict sections 5.1 and 9.4.
		if (address.block_type() != xc7series::BlockType::CLB_IO_CLK ||
		    words.size() != Series7::words_per_frame) {
			if (words.size() > kEccWord && words[kEccWord] != 0)
				++rep.verbatim_frames_with_nonzero_word50;
			++rep.verbatim_frames;
			pass1.emplace(address, words);
			continue;
		}
		++rep.clb_frames;

		const auto it = static_frames.find(address);
		if (have_static &&
		    (it == static_frames.end() ||
		     it->second.size() != Series7::words_per_frame)) {
			std::ostringstream err;
			err << "The pre-pass needs the static's own frame at 0x"
			    << std::hex << std::setw(8) << std::setfill('0')
			    << static_cast<uint32_t>(address) << std::dec
			    << " for its STATIC term, and the static frame "
			       "set ";
			if (it == static_frames.end())
				err << "has no frame there";
			else
				err << "carries " << it->second.size()
				    << " words there, expected "
				    << Series7::words_per_frame;
			err << ". Pass 1 would drop the static's own routing "
			       "in "
			       "that frame. Give --static_frames every "
			       "CLB_IO_CLK frame the partial writes, or use "
			       "--prepass nostatic to accept an empty STATIC "
			       "term knowingly.";
			throw std::runtime_error(err.str());
		}
		const std::vector<uint32_t>& stat =
		    have_static ? it->second : kZeroFrame;

		const auto& masks = geometry.MasksFor(address);
		if (!masks.column_in_tilegrid)
			rep.columns_without_geometry.insert(
			    static_cast<uint32_t>(address) & ~0x7Fu);
		for (const std::string& t : *masks.unclassified_types)
			rep.unclassified_types_touched.insert(t);

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
			const uint32_t site = masks.site[w];
			const uint32_t park = ParkMask(minor, w);
			const uint32_t s = stat[w];
			out[w] = (words[w] & site) | park |
			         (s & ~site & ~park);
			rep.withheld_bits += static_cast<uint64_t>(
			    __builtin_popcount(words[w] & ~site & ~park));
			rep.inferred_cin_x1_bits += static_cast<uint64_t>(
			    __builtin_popcount(words[w] &
			                       masks.inferred_cin_x1[w] & ~park));
			rep.inferred_dsp_reg0_bits += static_cast<uint64_t>(
			    __builtin_popcount(words[w] &
			                       masks.inferred_dsp_reg0[w] &
			                       ~park));
		}
		// RECOMPUTE, do not copy -- on block type 0 only. Vivado's
		// pass 1 carries a correct ECC in 825 of 825 block-0 frames in
		// every module of both benches; the all-ones frames at minors
		// 24/25 look ECC-less only because the correct ECC of that
		// particular frame happens to be 0.
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

namespace {

bool Series7SameFrameGroup(const xc7series::FrameAddress& a,
                           const xc7series::FrameAddress& b) {
	return a.block_type() == b.block_type() &&
	       a.is_bottom_half_rows() == b.is_bottom_half_rows() &&
	       a.row() == b.row();
}

bool Series7FrameIsZero(const std::vector<uint32_t>& words) {
	for (uint32_t w : words) {
		if (w)
			return false;
	}
	return true;
}

}  // namespace

template <>
CompressedFrameData Configuration<Series7>::createCompressedFrameData(
    const Frames<Series7>::Frames2Data& frames,
    absl::optional<Series7::Part>& part) {
	using ArchType = Series7;
	const size_t kWordsPerFrame = ArchType::words_per_frame;

	std::vector<std::pair<ArchType::FrameAddress, const std::vector<uint32_t>*>>
	    seq;
	auto walk = absl::optional<ArchType::FrameAddress>(
	    ArchType::FrameAddress(0));
	while (walk) {
		auto it = frames.find(*walk);
		if (it == frames.end()) {
			std::ostringstream err;
			err << "Compressed full bitstream: the frame set is "
			       "missing address 0x"
			    << std::hex << static_cast<uint32_t>(*walk)
			    << std::dec
			    << ". Call Frames::addMissingFrames() first: the "
			       "encoding walks the whole device and cannot "
			       "leave a hole.";
			throw std::runtime_error(err.str());
		}
		if (it->second.size() != kWordsPerFrame)
			throw std::runtime_error(
			    "Compressed full bitstream: a frame is not "
			    "words_per_frame words long.");
		seq.emplace_back(*walk, &it->second);
		walk = part->GetNextFrameAddress(*walk);
	}
	if (seq.size() != frames.size()) {
		std::ostringstream err;
		err << "Compressed full bitstream: the frame set carries "
		    << frames.size() << " address(es) and the part walks "
		    << seq.size()
		    << ". An address the part does not have cannot be written.";
		throw std::runtime_error(err.str());
	}

	CompressedFrameData blocks;
	const size_t n = seq.size();
	size_t i = 0;
	while (i < n) {
		size_t j = i;
		while (j < n &&
		       Series7SameFrameGroup(seq[i].first, seq[j].first) &&
		       !Series7FrameIsZero(*seq[j].second))
			++j;
		size_t k = j;
		while (k < n &&
		       Series7SameFrameGroup(seq[i].first, seq[k].first) &&
		       Series7FrameIsZero(*seq[k].second))
			++k;

		CompressedFrameBlock block;
		block.far = static_cast<uint32_t>(seq[i].first);
		if (k > j) {
			for (size_t m = i; m <= j; ++m)
				block.words.insert(block.words.end(),
				                   seq[m].second->begin(),
				                   seq[m].second->end());
			block.mfw = true;
			for (size_t m = j + 1; m < k; ++m)
				block.replay.push_back(
				    static_cast<uint32_t>(seq[m].first));
		} else {
			for (size_t m = i; m < j; ++m)
				block.words.insert(block.words.end(),
				                   seq[m].second->begin(),
				                   seq[m].second->end());
			block.words.insert(block.words.end(),
			                   2 * kWordsPerFrame, 0);
			block.mfw = false;
		}
		blocks.push_back(std::move(block));
		i = k;
	}
	return blocks;
}

template <>
size_t Configuration<Series7>::checkCompressedFrameSet(
    const CompressedFrameData& blocks,
    const Frames<Series7>::Frames2Data& frames,
    absl::optional<Series7::Part>& part) {
	using ArchType = Series7;
	const size_t kWordsPerFrame = ArchType::words_per_frame;

	std::map<uint32_t, std::vector<uint32_t>> written;
	auto commit = [&written](uint32_t address,
	                         const std::vector<uint32_t>& words) {
		if (!written.emplace(address, words).second) {
			std::ostringstream err;
			err << "Compressed full bitstream: address 0x"
			    << std::hex << address << std::dec
			    << " is written twice.";
			throw std::runtime_error(err.str());
		}
	};

	for (const auto& block : blocks) {
		if (block.words.empty() ||
		    block.words.size() % kWordsPerFrame != 0)
			throw std::runtime_error(
			    "Compressed full bitstream: an FDRI burst is not a "
			    "whole, non-zero number of frames.");
		const size_t burst_frames = block.words.size() / kWordsPerFrame;
		ArchType::FrameAddress cursor(block.far);
		if (frames.find(cursor) == frames.end()) {
			std::ostringstream err;
			err << "Compressed full bitstream: block FAR 0x"
			    << std::hex << block.far << std::dec
			    << " is not a frame address of this part.";
			throw std::runtime_error(err.str());
		}
		if (!block.mfw && !block.replay.empty())
			throw std::runtime_error(
			    "Compressed full bitstream: a block with no MFW "
			    "episode carries replay addresses, which nothing "
			    "would emit.");

		const size_t auto_incremented =
		    block.mfw ? burst_frames - 1 : burst_frames - 2;
		if (!block.mfw && burst_frames < 3)
			throw std::runtime_error(
			    "Compressed full bitstream: a row-end block needs "
			    "at least one real frame and two pad slots.");

		for (size_t f = 0; f < auto_incremented; ++f) {
			commit(static_cast<uint32_t>(cursor),
			       std::vector<uint32_t>(
			           block.words.begin() + f * kWordsPerFrame,
			           block.words.begin() +
			               (f + 1) * kWordsPerFrame));
			auto next = part->GetNextFrameAddress(cursor);
			const bool last = (f + 1 == auto_incremented);
			const bool in_group =
			    next && Series7SameFrameGroup(cursor, *next);
			if (block.mfw || !last) {
				if (!in_group) {
					std::ostringstream err;
					err << "Compressed full bitstream: an "
					       "FDRI burst crosses a row or "
					       "block-type boundary at 0x"
					    << std::hex
					    << static_cast<uint32_t>(cursor)
					    << std::dec
					    << ". The frame-address counter has "
					       "two dummy positions there and "
					       "the burst's remaining frames "
					       "would land on the wrong "
					       "addresses.";
					throw std::runtime_error(err.str());
				}
				cursor = *next;
			} else if (in_group) {
				std::ostringstream err;
				err << "Compressed full bitstream: a row-end "
				       "block's two pad slots would land on "
				       "real frame address 0x"
				    << std::hex
				    << static_cast<uint32_t>(*next) << std::dec
				    << " rather than in the counter's dummy "
				       "positions.";
				throw std::runtime_error(err.str());
			}
		}

		if (block.mfw) {
			const std::vector<uint32_t> seed(
			    block.words.end() - kWordsPerFrame,
			    block.words.end());
			commit(static_cast<uint32_t>(cursor), seed);
			for (uint32_t address : block.replay)
				commit(address, seed);
		}
	}

	if (written.size() != frames.size()) {
		std::ostringstream err;
		err << "Compressed full bitstream: the blocks commit "
		    << written.size() << " frame(s) and the input carries "
		    << frames.size() << ".";
		throw std::runtime_error(err.str());
	}
	for (const auto& frame : frames) {
		auto it = written.find(static_cast<uint32_t>(frame.first));
		if (it == written.end()) {
			std::ostringstream err;
			err << "Compressed full bitstream: address 0x"
			    << std::hex << static_cast<uint32_t>(frame.first)
			    << std::dec << " is never written.";
			throw std::runtime_error(err.str());
		}
		if (it->second != frame.second) {
			std::ostringstream err;
			err << "Compressed full bitstream: address 0x"
			    << std::hex << static_cast<uint32_t>(frame.first)
			    << std::dec
			    << " is written with words the input does not "
			       "carry.";
			throw std::runtime_error(err.str());
		}
	}
	return written.size();
}

template <>
void Configuration<Series7>::createConfigurationPackage(
    Series7::ConfigurationPackage& out_packets,
    const PacketData& packet_data,
    absl::optional<Series7::Part>& part,
    const FullStreamOptions& options) {
	using ArchType = Series7;
	using ConfigurationRegister = ArchType::ConfRegType;

	// THE TWO SUBSTITUTION POINTS.
	//
	// A Vivado FULL bitstream for this family writes the CRC register
	// TWICE, and the two positions are read out of one rather than chosen:
	// in pt_v2_soc_fn.bit (Vivado 2025.2, 7a100tfgg484) the sequence is
	//
	//     ... FDRI/MFWR ...  CRC <= 0x10c41b1c  CMD:GRESTORE  CMD:DGHIGH
	//     MASK  CTL1  CMD:START  FAR <= 0x03be0000  MASK  CTL0
	//     CRC <= 0x615009a6  CMD:DESYNC
	//
	// which is, position for position, where THIS writer has always put two
	// CMD:RCRC writes. That is not a coincidence: it is exactly the
	// two-word substitution Vivado's BITSTREAM.GENERAL.CRC=DISABLE
	// performs, the same one the partial path documents
	// (docs/partial-bitstream.md section 11). 0x30000001,<crc> becomes
	// 0x30008001,0x00000007 and nothing else in the stream moves.
	//
	// So options.emit_crc does not add a code path; it declines to disable
	// one.
	//
	// computeStreamCrc() is called on out_packets AS IT STANDS, which is
	// why this is a lambda and not a precomputed pair of values: the CRC
	// register holds the accumulator over every register write since the
	// last reset, and a write to CRC is itself a reset, so CRC #2 covers
	// only what follows CRC #1. Reading the accumulator at the moment of
	// emission is the only formulation that cannot get that wrong.
	auto crc_or_rcrc = [&out_packets, &options]() {
		if (options.emit_crc) {
			out_packets.emplace_back(
			    new ConfigurationPacketWithPayload<
			        1, ConfigurationRegister>(
			        ConfigurationPacket<ConfigurationRegister>::
			            Opcode::Write,
			        ConfigurationRegister::CRC,
			        {Configuration<Series7>::computeStreamCrc(
			            out_packets)}));
			return;
		}
		out_packets.emplace_back(
		    new ConfigurationPacketWithPayload<1,
		                                       ConfigurationRegister>(
		        ConfigurationPacket<ConfigurationRegister>::Opcode::
		            Write,
		        ConfigurationRegister::CMD,
		        {static_cast<uint32_t>(xc7series::Command::RCRC)}));
	};

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

	// Configuration Options 1. 0x00000000 unless the caller asked for
	// something else: that is every documented COR1 field at its documented
	// default (UG470 Table 5-32), and it is the word in all 20 Vivado
	// full-bitstream packet dumps in the xc7-dpr corpus.
	//
	// THE FIELD THIS EXISTS FOR IS BIT 8, RBCRC_EN. With it clear -- which
	// is what every full bitstream this writer has ever emitted carries --
	// the device's continuous background readback of configuration memory
	// never starts, so nothing on the die is watching for an upset between
	// loads. It is unreachable from a hardcoded constant, which is the
	// whole reason FullStreamOptions::cor1 exists.
	out_packets.emplace_back(
	    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
	        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
	        ConfigurationRegister::COR1, {options.cor1.value_or(0x0)}));
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
	auto far_write = [&out_packets](uint32_t address) {
		out_packets.emplace_back(
		    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
		        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
		        ConfigurationRegister::FAR, {address}));
	};
	auto cmd_write = [&out_packets](xc7series::Command command) {
		out_packets.emplace_back(
		    new ConfigurationPacketWithPayload<1, ConfigurationRegister>(
		        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
		        ConfigurationRegister::CMD,
		        {static_cast<uint32_t>(command)}));
	};
	auto fdri_burst = [&out_packets](const PacketData& words) {
		out_packets.emplace_back(
		    new ConfigurationPacket<ConfigurationRegister>(
		        TYPE1,
		        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
		        ConfigurationRegister::FDRI, {}));
		out_packets.emplace_back(
		    new ConfigurationPacket<ConfigurationRegister>(
		        TYPE2,
		        ConfigurationPacket<ConfigurationRegister>::Opcode::Write,
		        ConfigurationRegister::FDRI, words));
	};

	if (options.compressed) {
		bool first_block = true;
		for (const CompressedFrameBlock& block : *options.compressed) {
			if (first_block) {
				far_write(block.far);
				cmd_write(xc7series::Command::WCFG);
			} else {
				cmd_write(xc7series::Command::WCFG);
				far_write(block.far);
			}
			first_block = false;
			out_packets.emplace_back(
			    new NopPacket<ConfigurationRegister>());
			fdri_burst(block.words);
			if (!block.mfw)
				continue;
			cmd_write(xc7series::Command::MFW);
			for (int ii = 0; ii < 12; ++ii)
				out_packets.emplace_back(
				    new NopPacket<ConfigurationRegister>());
			out_packets.emplace_back(
			    new ConfigurationPacketWithPayload<
			        8, ConfigurationRegister>(
			        ConfigurationPacket<ConfigurationRegister>::
			            Opcode::Write,
			        ConfigurationRegister::MFWR,
			        {0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0}));
			for (uint32_t address : block.replay) {
				far_write(address);
				out_packets.emplace_back(
				    new ConfigurationPacketWithPayload<
				        4, ConfigurationRegister>(
				        ConfigurationPacket<
				            ConfigurationRegister>::Opcode::
				            Write,
				        ConfigurationRegister::MFWR,
				        {0x0, 0x0, 0x0, 0x0}));
			}
		}
	} else {
		far_write(0x0);
		cmd_write(xc7series::Command::WCFG);
		out_packets.emplace_back(
		    new NopPacket<ConfigurationRegister>());
		fdri_burst(packet_data);
	}

	// Finalization sequence.
	// CRC #1 -- immediately after the frame data, before CMD:GRESTORE.
	crc_or_rcrc();
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
	// CRC #2 -- after the park FAR and the MASK/CTL0 pair, before
	// CMD:DESYNC. It accumulates only what follows CRC #1, because a write
	// to the CRC register resets the accumulator.
	crc_or_rcrc();
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
    absl::optional<UltraScale::Part>& part,
    const FullStreamOptions& options) {
	// COR1 IS THE ONE FULL-BITSTREAM REGISTER THIS WRITER OFFERS A KNOB ON,
	// AND THE KNOB IS SERIES7-ONLY. UltraScale's COR1 default here is
	// 0x00400000 and its layout is UG570's, which this project has never
	// measured.
	if (options.compressed) {
		throw std::runtime_error(
		    "FullStreamOptions::compressed is the multiple-frame-write "
		    "encoding, and its grammar was read out of Vivado's own "
		    "7-series compressed bitstreams. No other architecture here "
		    "has been measured, and an MFWR sequence guessed from this "
		    "one would emit at exit 0.");
	}

	if (options.cor1) {
		throw std::runtime_error(
		    "FullStreamOptions::cor1 is characterised for Series7 "
		    "only. UltraScale's COR1 layout is UG570's and is "
		    "uncharacterised here. "
		    "There is no degraded version to fall back to: a 7-series "
		    "COR1 field pattern written into this register would be a "
		    "guess that emits at exit 0.");
	}

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
    absl::optional<UltraScalePlus::Part>& part,
    const FullStreamOptions& options) {
	// COR1 IS THE ONE FULL-BITSTREAM REGISTER THIS WRITER OFFERS A KNOB ON,
	// AND THE KNOB IS SERIES7-ONLY. UltraScale+'s COR1 default here is
	// 0x00400000 and its layout is UG570's, which this project has never
	// measured.
	if (options.compressed) {
		throw std::runtime_error(
		    "FullStreamOptions::compressed is the multiple-frame-write "
		    "encoding, and its grammar was read out of Vivado's own "
		    "7-series compressed bitstreams. No other architecture here "
		    "has been measured, and an MFWR sequence guessed from this "
		    "one would emit at exit 0.");
	}

	if (options.cor1) {
		throw std::runtime_error(
		    "FullStreamOptions::cor1 is characterised for Series7 "
		    "only. UltraScale+'s COR1 layout is UG570's and is "
		    "uncharacterised here. "
		    "There is no degraded version to fall back to: a 7-series "
		    "COR1 field pattern written into this register would be a "
		    "guess that emits at exit 0.");
	}

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
