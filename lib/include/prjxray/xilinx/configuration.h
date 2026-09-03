/*
 * Copyright (C) 2017-2020  The Project X-Ray Authors.
 *
 * Use of this source code is governed by a ISC-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/ISC
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef PRJXRAY_LIB_XILINX_CONFIGURATION_H_
#define PRJXRAY_LIB_XILINX_CONFIGURATION_H_

#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/types/span.h>

#include <prjxray/bit_ops.h>
#include <prjxray/xilinx/architectures.h>
#include <prjxray/xilinx/frames.h>
#include <prjxray/xilinx/xc7series/prepass_geometry.h>

namespace prjxray {
namespace xilinx {

// What createPrePassFrames did, in numbers, so the caller can print it. Two of
// these counters exist because docs/far-double-write-verdict.md section 5.1
// flags two elements of the SITE mask as INFERRED rather than measured: the
// SLICE*_X1 half of the PRECYINIT.CIN exclusion (bit 31_12, never seen in a
// bitstream) and the four {A,B}REG_0 minor-27 DSP bits. If a future golden ever
// disagrees with this writer, the first question is whether an inferred element
// fired, and a counter answers it where a comment cannot.
struct PrePassReport {
	size_t clb_frames = 0;
	size_t verbatim_frames = 0;

	// pass-2 bits that pass 1 withholds, and the two inferred subsets.
	uint64_t withheld_bits = 0;
	uint64_t inferred_cin_x1_bits = 0;
	uint64_t inferred_dsp_reg0_bits = 0;

	// Block-0 frames whose configuration column has no tile at all in
	// tilegrid.json, so SITE was all zeros for want of geometry. 12 of
	// xc7a100t's 234 block-0 columns are in that state.
	std::set<uint32_t> columns_without_geometry;

	// Bit-bearing tile types covered by the frames written that section
	// 5.1's table does not name. They contribute nothing to SITE under
	// "anything else -> nothing", which no bench has exercised.
	std::set<std::string> unclassified_types_touched;

	// Vivado writes no frame ECC in a non-CLB_IO_CLK block: word 50 is
	// zero in 772 of 772. A nonzero one in the input is not this writer's
	// doing -- it copies the word verbatim -- but it means the input did
	// not come from where the measurement did.
	size_t verbatim_frames_with_nonzero_word50 = 0;
};

// Knobs on the emitted partial-bitstream packet stream. Every default is the
// value measured in Vivado 2025.2's own 7-series partial bitstreams
// (gateware/vivado-dfx-golden/golden/packets/rm_*_partial.packets.txt); see
// docs/partial-bitstream.md for the word-level map they were read from.
struct PartialStreamOptions {
	// A .bit loaded over JTAG carries an IDCODE write; a raw stream fed to
	// the ICAP does not. False is --raw.
	bool emit_idcode = true;

	// True: compute the stream CRC and write it to the CRC register.
	// False: substitute a CMD:RCRC write in its place, which is exactly
	// what Vivado's BITSTREAM.GENERAL.CRC=DISABLE does -- measured against
	// the crcdisable goldens, the two streams differ in exactly two 32-bit
	// words per CRC write (0x30000001,<crc> -> 0x30008001,0x00000007) and
	// in nothing else. A substitution, not a second code path.
	bool compute_crc = true;

	// NOPs after CMD:DESYNC. 16 in every golden partial.
	unsigned int trailing_nops = 16;
};

template <typename ArchType>
class Configuration {
       public:
	using FrameMap = std::map<typename ArchType::FrameAddress,
	                          absl::Span<const uint32_t>>;
	using PacketData = std::vector<uint32_t>;

	// Returns a configuration, i.e. collection of frame addresses
	// and corresponding data from a collection of configuration packets.
	template <typename Collection>
	static absl::optional<Configuration<ArchType>> InitWithPackets(
	    const typename ArchType::Part& part,
	    Collection& packets);

	// Creates the complete configuration package which is later on
	// used by the bitstream writer to generate the bitstream file.
	// The pacakge forms a sequence suitable for Xilinx devices.
	// The programming sequence for Series-7 is taken from
	// https://www.kc8apf.net/2018/05/unpacking-xilinx-7-series-bitstreams-part-2/
	//
	// emit_crc = false (the default, and what every stream this writer has
	// ever emitted does): the finalization sequence carries TWO CMD:RCRC
	// writes and NO write to the CRC register, so the device's own CRC
	// comparison never runs. That is bit-for-bit what Vivado's
	// BITSTREAM.GENERAL.CRC=DISABLE emits.
	//
	// emit_crc = true: each of those two CMD:RCRC writes is replaced, in
	// place, by a Type-1 write of computeStreamCrc() over everything that
	// precedes it. The two positions are not chosen -- they are where a
	// Vivado full bitstream for this family puts its two CRC writes:
	// immediately after the frame data and immediately before CMD:DESYNC.
	// A two-word substitution per CRC, exactly as on the partial path;
	// nothing else in the stream moves.
	//
	// It is OPT-IN, and deliberately so. Flipping the default would change
	// the payload of every full bitstream this toolchain has ever built and
	// invalidate every recorded payload hash that identifies one.
	static void createConfigurationPackage(
	    typename ArchType::ConfigurationPackage& out_packets,
	    const PacketData& packet_data,
	    absl::optional<typename ArchType::Part>& part,
	    bool emit_crc);

	// The payload of a partial bitstream, DERIVED FROM THE INPUT FRAME SET.
	//
	// One entry per maximal run of frames that are consecutive under
	// Part::GetNextFrameAddress: {address of the run's first frame, the
	// run's words}. One FAR + FDRI block is emitted per entry, so the
	// device's frame-address counter never steps over a frame the caller
	// did not ask for, and no frame outside the input .frm can be written.
	// That is a structural property of the shape, not a check bolted on
	// afterwards -- there is no code path that could add a frame.
	using PartialPacketData = std::vector<std::pair<uint32_t, PacketData>>;

	// Splits `frames` into those runs and TERMINATES each run, so that
	// every frame of the input reaches configuration memory.
	//
	// THE DEVICE HOLDS ONE FRAME BACK. N frames shifted into one FDRI
	// burst commit N-1, leaving the frame-address counter at start+N-1;
	// the pending frame is then DISCARDED by the next FAR write or by
	// DESYNC. Measured on this bench by ICAP at N = 2, 3 and 4, on two
	// independent instruments that name the same committed set -- the
	// in-band FAR read and the out-of-band readback
	// (docs/icap-rung4-fdri-and-the-flush-frame.md sections 8, 10 and 12).
	// A burst carrying nothing past its last real frame loses that frame.
	//
	// A block ending AT a row or block-type boundary is ALREADY
	// terminated, and that path is unchanged. The device's frame-address
	// counter has two dummy positions there, `pad_frames` zero frames land
	// in them and land nowhere -- and the FIRST of them is what pushes the
	// block's last real frame out. That object is the ROW-END PAD SLOT and
	// `--pad_frames` is its knob.
	//
	// A block ending MID-ROW has no dummy position to fall into, and until
	// 2026-09-03 got nothing at all. It now gets exactly one FLUSH FRAME,
	// at GetNextFrameAddress(last), taken from `flush_source`. That is a
	// DIFFERENT OBJECT from the pad slot; the three-object table is
	// docs/s1-byteman-adoption-decision.md section 2.1.
	//
	// THE MEASUREMENT THIS COMMENT USED TO STATE BACKWARDS. In the Vivado
	// 2025.2 golden partial for xc7a100tcsg324-1
	// (gateware/vivado-dfx-golden/):
	//   CLB  block  83325 words = 825 frames over a pblock that owns 824.
	//                The 825th lands at 0x00001C00 -- row 0 column 56,
	//                which is OUTSIDE the pblock's columns 32-55. It is
	//                the FLUSH FRAME. The old text read that address as
	//                the block's own last frame and concluded "MID-ROW,
	//                and it carries NO pad"; the block does end mid-row
	//                and it is terminated, by one frame, not by a pad.
	//   BRAM block  38986 words = 386 frames = 384 real frames ending at
	//                row 0 col 3 minor 127 -- a ROW END -- PLUS exactly
	//                two zero frames, which ARE pad slots.
	// +1 mid-row and +2 at a row end, identical across all four RMs of
	// both gateware/vivado-dfx-golden/golden/ and golden-fgg484-2/.
	//
	// AND THE OMISSION WAS LOAD-BEARING, measured on silicon 2026-09-03:
	// a 72-frame ROI whose last frame is 0x000010A3 loaded twice over a
	// resident static, and the 72-frame readback found 71 committed both
	// times with 0x000010A3 still holding the static's own content --
	// written ZERO times, not written wrongly.
	//
	// THE FLUSH FRAME'S CONTENT IS THE STATIC'S OWN, AND THERE IS NO
	// FALLBACK. Under the measured hold-one-back model it never commits
	// and its content cannot matter. That model is measured at three burst
	// lengths, not proven in general; if the depth were ever 0 the flush
	// frame LANDS, and an all-zero one would then write zeros over live
	// static configuration at an address the caller never asked to write.
	// For the ROI above the flush frame is 0x00001100, where the resident
	// static holds three non-zero words (two routing words plus its frame
	// ECC). So the content comes from `flush_source` and a missing address
	// is a REFUSAL. This is Vivado's policy, and it is the only one that
	// is safe under both models.
	//
	// Throws std::runtime_error if any input address is not a valid frame
	// address on `part`, or if a block ends mid-row and `flush_source`
	// carries no frame at the address the flush frame would occupy.
	static PartialPacketData createPartialPacketData(
	    const typename Frames<ArchType>::Frames2Data& frames,
	    const typename Frames<ArchType>::Frames2Data& flush_source,
	    absl::optional<typename ArchType::Part>& part,
	    unsigned int pad_frames);

	// Re-derives the frame set the emitted blocks will land on -- by
	// walking GetNextFrameAddress FORWARD from each block's FAR, which is
	// the device's own rule and a different code path from the grouping
	// above -- and throws unless it is exactly `frames`. Returns the count
	// of REAL frames, which is what --expected_frames is compared against;
	// a flush frame is not one and is not counted.
	//
	// This is Phase 6 Unit 6.3 step 5: `fasm2frames --roi` is a union
	// rather than a filter, so an ROI leak is silent, and the deferred half
	// of that fix was to make the writer check the frame set on EVERY
	// emission rather than only when someone passes a flag. Hence: called
	// unconditionally, no flag to forget.
	//
	// IT ALSO CHECKS THE TERMINATION, and until 2026-09-03 it could not.
	// The only rule it had, `frames_in_block - i != pad_frames`, is reached
	// through `at_end` -- so it fires only for a block that ends at a row
	// or block-type boundary, and every ROI this tree has built ends
	// mid-row. A stream that dropped its last frame on the device passed it
	// 14 times. The rule now covers both terminations:
	//
	//   ends at a row/block-type end:  trailing frames == pad_frames
	//   ends mid-row:                  trailing frames == 1, that frame is
	//                                  at GetNextFrameAddress(last real),
	//                                  and its words equal `flush_source`'s
	//                                  at that address
	//
	// The second conjunct of the mid-row arm is why `flush_source` is an
	// argument here as well as to the writer: a checker that only counted
	// would accept an all-zero flush frame, which is the one payload that
	// can destroy static configuration if the hold-one-back model is ever
	// wrong.
	//
	// `flush_addresses`, when non-null, is filled with the address of every
	// flush frame the stream writes. The caller needs it because those
	// addresses are OUTSIDE the input .frm and a round-trip verifier that
	// did not know them would report each one as a frame the caller never
	// asked for -- the correct verdict for every other extra address, and
	// the wrong one for these.
	static size_t checkPartialFrameSet(
	    const PartialPacketData& blocks,
	    const typename Frames<ArchType>::Frames2Data& frames,
	    const typename Frames<ArchType>::Frames2Data& flush_source,
	    absl::optional<typename ArchType::Part>& part,
	    unsigned int pad_frames,
	    std::set<uint32_t>* flush_addresses = nullptr);

	// Builds the FAR double write's FIRST pass from the caller's frames.
	//
	// A 7-series partial bitstream written by Vivado writes every FAR block
	// TWICE, with different payloads, and the two passes are interleaved
	// A B A B -- pass 1 of every block, then pass 2 of every block. Pass 2
	// is the configuration; pass 1 is the same region with the interconnect
	// withheld. `pass2` here is the caller's .frm, which for a DFX writer
	// is already STATIC | RM.
	//
	// The rule, from docs/far-double-write-verdict.md section 5.1 (measured
	// against Vivado 2025.2 on xc7a100tcsg324-1, ten reconfigurable modules
	// across two locked statics, plus the original four):
	//
	//   block type 0:  pass1[w] = STATIC[w] | (RM[w] & SITE(tile,m,w))
	//                             | PARK(m,w)
	//                  then, per frame, recompute word 50 bits 12:0
	//   any other:     pass1[w] = pass2[w]   verbatim, ECC word included
	//
	// and equivalently, for a writer that holds pass 2 rather than the RM,
	//
	//   pass1[w] = (pass2[w] & SITE) | PARK | (STATIC[w] & ~SITE & ~PARK)
	//
	// `static_frames` supplies STATIC. It is NOT recoverable from pass 2 --
	// the union has destroyed each bit's provenance -- which is why it is a
	// separate input; see docs/partial-bitstream.md. When `have_static` is
	// false the STATIC term is dropped, which is wrong in exactly the bits
	// that carry the static's own routing inside the region (84 bits in 67
	// words behind the golden's static, 142 in 121 behind the second one),
	// so the tool must not do that silently.
	//
	// SITE IS PER TILE, not per (minor, word) -- see PrePassGeometry, which
	// supplies it. The predecessor of this function used
	// SiteMask(minor, word) and was wrong by 2,987 bits in one module of
	// the round-2 corpus, everywhere off the CLB columns.
	//
	// BLOCK TYPE. The transformation applies to block type 0 (CLB_IO_CLK)
	// and to nothing else. Every other block type is written twice
	// verbatim: same payload, ECC word included, no mask, no park, no
	// recomputation. Block type 1 (BLOCK_RAM) also has minors 24 and 25,
	// and applying PARK there forces whole BRAM frames to all-ones -- six
	// of them in the measured golden. And prjxray's updateECC must NOT run
	// there: word 50 of every BLOCK_RAM frame is zero in both passes, 0
	// nonzero of 772, and updateECC would invent one in exactly the 16
	// frames per module that carry initialised content.
	static typename Frames<ArchType>::Frames2Data createPrePassFrames(
	    const typename Frames<ArchType>::Frames2Data& pass2,
	    const typename Frames<ArchType>::Frames2Data& static_frames,
	    bool have_static,
	    const xc7series::PrePassGeometry& geometry,
	    PrePassReport* report);

	// The bitstream CRC over an already-built configuration package, up to
	// but not including the CRC write itself.
	//
	// Reflected CRC-32C, polynomial 0x82F63B78, fed data[31:0] LSB-first
	// then addr[4:0] LSB-first (prjxray xc7series/crc.h). Every register
	// write is accumulated except a write to the CRC register; CMD:RCRC
	// resets the accumulator and its own write is consumed by the reset.
	// IEEE 802.3 is refuted: it reproduces none of the six vendor values.
	// See docs/vivado-dfx-oracle.md section 3.
	static uint32_t computeStreamCrc(
	    const typename ArchType::ConfigurationPackage& packets);

	// Creates the configuration package for partial configuration
	// which is later on used by the bitstream writer to generate
	// the partial bitstream file.
	//
	// cfg_packets is taken BY CONST REFERENCE and must outlive
	// `out_packets`. ConfigurationPacket stores a non-owning
	// absl::Span<const uint32_t> over its payload; taking the container by
	// value here (as this function originally did) freed every FDRI payload
	// at return and left BitstreamWriter reading 968,388 bytes of freed
	// heap -- confirmed by AddressSanitizer, and the same defect class as
	// the bitread span-lifetime crash. Same fix as there: the owner lives
	// in the caller's scope, not in a scope that ends first.
	//
	// To emit the FAR double write, pass the concatenation of pass 1's
	// blocks and pass 2's blocks: the per-block emission is identical, so
	// concatenation IS the A B A B interleave.
	static void createPartialConfigurationPackage(
	    typename ArchType::ConfigurationPackage& out_packets,
	    const PartialPacketData& cfg_packets,
	    absl::optional<typename ArchType::Part>& part,
	    const PartialStreamOptions& options);

	// Returns the payload for a type 2 packet
	// which allows for bigger payload compared to type 1.
	static PacketData createType2ConfigurationPacketData(
	    const typename Frames<ArchType>::Frames2Data& frames,
	    absl::optional<typename ArchType::Part>& part);

	Configuration(const typename ArchType::Part& part,
	              std::map<typename ArchType::FrameAddress,
	                       std::vector<uint32_t>>* frames)
	    : part_(part) {
		for (auto& frame : *frames) {
			frames_[frame.first] =
			    absl::Span<const uint32_t>(frame.second);
		}
	}

	Configuration(const typename ArchType::Part& part,
	              const FrameMap& frames)
	    : part_(part), frames_(std::move(frames)) {}

	const typename ArchType::Part& part() const { return part_; }
	const FrameMap& frames() const { return frames_; }
	void PrintFrameAddresses(FILE* fp);

       private:
	typename ArchType::Part part_;
	FrameMap frames_;
};

// The explicit specializations below are defined in configuration.cc.
// They must be declared before use in every translation unit
// ([temp.expl.spec]): without these declarations, a TU that calls
// createType2ConfigurationPacketData<Spartan6> instantiates the primary
// template defined further down in this header, and that COMDAT copy
// collides with the strong definition from configuration.cc when linking
// with mingw-w64 ld ("multiple definition"); ELF linkers silently
// resolve the collision in favour of the strong symbol.
template <>
Configuration<Spartan6>::PacketData
Configuration<Spartan6>::createType2ConfigurationPacketData(
    const Frames<Spartan6>::Frames2Data& frames,
    absl::optional<Spartan6::Part>& part);

template <>
void Configuration<Spartan6>::createConfigurationPackage(
    Spartan6::ConfigurationPackage& out_packets,
    const PacketData& packet_data,
    absl::optional<Spartan6::Part>& part,
    bool emit_crc);

template <>
void Configuration<Series7>::createConfigurationPackage(
    Series7::ConfigurationPackage& out_packets,
    const PacketData& packet_data,
    absl::optional<Series7::Part>& part,
    bool emit_crc);

template <>
void Configuration<UltraScale>::createConfigurationPackage(
    UltraScale::ConfigurationPackage& out_packets,
    const PacketData& packet_data,
    absl::optional<UltraScale::Part>& part,
    bool emit_crc);

template <>
void Configuration<UltraScalePlus>::createConfigurationPackage(
    UltraScalePlus::ConfigurationPackage& out_packets,
    const PacketData& packet_data,
    absl::optional<UltraScalePlus::Part>& part,
    bool emit_crc);

// Same reason as above: these four are defined in configuration.cc and must be
// declared before use in every translation unit.
template <>
void Configuration<Spartan6>::createPartialConfigurationPackage(
    Spartan6::ConfigurationPackage& out_packets,
    const PartialPacketData& cfg_packets,
    absl::optional<Spartan6::Part>& part,
    const PartialStreamOptions& options);

template <>
void Configuration<Series7>::createPartialConfigurationPackage(
    Series7::ConfigurationPackage& out_packets,
    const PartialPacketData& cfg_packets,
    absl::optional<Series7::Part>& part,
    const PartialStreamOptions& options);

template <>
void Configuration<UltraScale>::createPartialConfigurationPackage(
    UltraScale::ConfigurationPackage& out_packets,
    const PartialPacketData& cfg_packets,
    absl::optional<UltraScale::Part>& part,
    const PartialStreamOptions& options);

template <>
void Configuration<UltraScalePlus>::createPartialConfigurationPackage(
    UltraScalePlus::ConfigurationPackage& out_packets,
    const PartialPacketData& cfg_packets,
    absl::optional<UltraScalePlus::Part>& part,
    const PartialStreamOptions& options);

// The pre-pass and the stream CRC are 7-series only: both are measured on
// xc7a100tcsg324-1 and neither has been characterised anywhere else. The
// primary templates below throw, so an unsupported architecture fails loudly
// rather than emitting a plausible-looking guess.
template <>
Frames<Series7>::Frames2Data Configuration<Series7>::createPrePassFrames(
    const Frames<Series7>::Frames2Data& pass2,
    const Frames<Series7>::Frames2Data& static_frames,
    bool have_static,
    const xc7series::PrePassGeometry& geometry,
    PrePassReport* report);

template <>
uint32_t Configuration<Series7>::computeStreamCrc(
    const Series7::ConfigurationPackage& packets);

template <typename ArchType>
typename Frames<ArchType>::Frames2Data
Configuration<ArchType>::createPrePassFrames(
    const typename Frames<ArchType>::Frames2Data& pass2,
    const typename Frames<ArchType>::Frames2Data& static_frames,
    bool have_static,
    const xc7series::PrePassGeometry& geometry,
    PrePassReport* report) {
	throw std::runtime_error(
	    "The FAR double-write pre-pass is characterised for 7-series only. "
	    "docs/far-double-write-verdict.md section 6 records what was not "
	    "tested; guessing it for another architecture would emit a "
	    "structurally wrong partial bitstream at exit 0.");
}

template <typename ArchType>
uint32_t Configuration<ArchType>::computeStreamCrc(
    const typename ArchType::ConfigurationPackage& packets) {
	throw std::runtime_error(
	    "The bitstream CRC model is measured for 7-series only.");
}

template <typename ArchType>
typename Configuration<ArchType>::PacketData
Configuration<ArchType>::createType2ConfigurationPacketData(
    const typename Frames<ArchType>::Frames2Data& frames,
    absl::optional<typename ArchType::Part>& part) {
	PacketData packet_data;
	// Certain configuration frames blocks are separated by Zero Frames,
	// i.e. frames with words with all zeroes. For Series-7, US and US+
	// there zero frames separator consists of two frames.
	static const int kZeroFramesSeparatorWords =
	    ArchType::words_per_frame * 2;
	for (auto& frame : frames) {
		std::copy(frame.second.begin(), frame.second.end(),
		          std::back_inserter(packet_data));

		auto next_address = part->GetNextFrameAddress(frame.first);
		if (next_address &&
		    (next_address->block_type() != frame.first.block_type() ||
		     next_address->is_bottom_half_rows() !=
		         frame.first.is_bottom_half_rows() ||
		     next_address->row() != frame.first.row())) {
			packet_data.insert(packet_data.end(),
			                   kZeroFramesSeparatorWords, 0);
		}
	}
	packet_data.insert(packet_data.end(), kZeroFramesSeparatorWords, 0);
	return packet_data;
}

template <typename ArchType>
typename Configuration<ArchType>::PartialPacketData
Configuration<ArchType>::createPartialPacketData(
    const typename Frames<ArchType>::Frames2Data& frames,
    const typename Frames<ArchType>::Frames2Data& flush_source,
    absl::optional<typename ArchType::Part>& part,
    unsigned int pad_frames) {
	PartialPacketData blocks;
	const size_t pad_words =
	    static_cast<size_t>(pad_frames) * ArchType::words_per_frame;

	// `expected` is the address the device's frame-address counter will be
	// at after the previous frame; a frame that is not at that address
	// starts a new FAR block.
	//
	// `boundary` is the second reason to start one, and it is NOT implied
	// by the first: GetNextFrameAddress walks straight across a row or
	// block-type boundary, so a run that spans one looks contiguous. It is
	// not. Between two rows the hardware's frame-address counter passes
	// through two dummy frames, which is why the full-bitstream writer
	// inserts two zero frames there -- and a pad frame in a PARTIAL
	// bitstream lands on live static logic. Splitting instead means each
	// FDRI block stays inside one row of one block type, the counter never
	// reaches a dummy position, and no pad frame is ever needed.
	// (Caught by ConfigurationTest.PartialPacketDataSplitsAtRowBoundary,
	// which failed on the first draft of this function.)
	bool have_expected = false;
	uint32_t expected = 0;
	bool boundary = false;

	// Terminates the block currently at blocks.back(). Called while
	// `boundary` / `have_expected` / `expected` still describe that block's
	// LAST frame -- the same convention the padding code used before it.
	//
	// Two terminations, and they are not the same object. At a row or
	// block-type end the counter's next positions are dummies, so
	// `pad_frames` zero frames flush the block and land nowhere. Mid-row
	// there is no dummy position, so the block is flushed by ONE frame
	// carrying the static's own configuration at the address the counter
	// is about to reach.
	auto terminate_block = [&]() {
		if (blocks.empty())
			return;
		if (!have_expected || boundary) {
			blocks.back().second.insert(
			    blocks.back().second.end(), pad_words, 0);
			return;
		}
		typename ArchType::FrameAddress flush_at(expected);
		auto it = flush_source.find(flush_at);
		if (it == flush_source.end()) {
			std::ostringstream err;
			err << "This partial bitstream has a FAR block that "
			       "ends in the MIDDLE of a row, so it needs one "
			       "flush frame at 0x"
			    << std::hex << std::setw(8) << std::setfill('0')
			    << expected << std::dec
			    << " -- the device holds one frame back and "
			       "without it the block's last real frame is "
			       "never committed. The static frame set carries "
			       "no frame at that address, so there is nothing "
			       "to write there. THERE IS NO ZERO FALLBACK: an "
			       "all-zero flush frame is safe only while the "
			       "hold-one-back model holds, and if it ever does "
			       "not, that frame lands and erases whatever "
			       "static configuration lives at 0x"
			    << std::hex << std::setw(8) << std::setfill('0')
			    << expected << std::dec
			    << ". Widen --static_frames by one frame past each "
			       "block's last.";
			throw std::runtime_error(err.str());
		}
		if (it->second.size() != ArchType::words_per_frame) {
			std::ostringstream err;
			err << "The static frame at 0x" << std::hex
			    << std::setw(8) << std::setfill('0') << expected
			    << std::dec << ", which this partial's flush frame "
			                   "must carry, has "
			    << it->second.size() << " words, expected "
			    << ArchType::words_per_frame << ".";
			throw std::runtime_error(err.str());
		}
		std::copy(it->second.begin(), it->second.end(),
		          std::back_inserter(blocks.back().second));
	};

	for (auto& frame : frames) {
		const uint32_t address = static_cast<uint32_t>(frame.first);
		if (!part->IsValidFrameAddress(frame.first)) {
			std::ostringstream err;
			err << "Frame address 0x" << std::hex << std::setw(8)
			    << std::setfill('0') << address
			    << " in the input .frm is not a valid frame "
			       "address on this part.";
			throw std::runtime_error(err.str());
		}
		if (frame.second.size() != ArchType::words_per_frame) {
			std::ostringstream err;
			err << "Frame 0x" << std::hex << std::setw(8)
			    << std::setfill('0') << address << " has "
			    << std::dec << frame.second.size()
			    << " words, expected "
			    << ArchType::words_per_frame << ".";
			throw std::runtime_error(err.str());
		}
		if (!have_expected || boundary || address != expected) {
			// `boundary` / `expected` still describe the PREVIOUS
			// frame here, which is what terminate_block() needs:
			// pads if that frame ended a row or bus, one flush
			// frame if it did not.
			terminate_block();
			blocks.emplace_back(address, PacketData());
		}
		std::copy(frame.second.begin(), frame.second.end(),
		          std::back_inserter(blocks.back().second));
		auto next = part->GetNextFrameAddress(frame.first);
		have_expected = static_cast<bool>(next);
		expected = have_expected ? static_cast<uint32_t>(*next) : 0;
		boundary =
		    have_expected &&
		    (next->block_type() != frame.first.block_type() ||
		     next->is_bottom_half_rows() !=
		         frame.first.is_bottom_half_rows() ||
		     next->row() != frame.first.row());
	}
	terminate_block();
	return blocks;
}

template <typename ArchType>
size_t Configuration<ArchType>::checkPartialFrameSet(
    const PartialPacketData& blocks,
    const typename Frames<ArchType>::Frames2Data& frames,
    const typename Frames<ArchType>::Frames2Data& flush_source,
    absl::optional<typename ArchType::Part>& part,
    unsigned int pad_frames,
    std::set<uint32_t>* flush_addresses) {
	std::set<uint32_t> landed;
	size_t duplicated = 0;
	for (auto& block : blocks) {
		const size_t frames_in_block =
		    block.second.size() / ArchType::words_per_frame;
		absl::optional<typename ArchType::FrameAddress> address(
		    typename ArchType::FrameAddress(block.first));
		size_t i = 0;
		bool at_end = false;
		// The leading run: frames the INPUT asked for. The first
		// address the input does not name ends it, and everything
		// from there is termination rather than payload.
		while (i < frames_in_block) {
			if (!address)
				throw std::runtime_error(
				    "A partial-bitstream block runs past the "
				    "last frame of the device.");
			if (!frames.count(*address))
				break;
			if (!landed.insert(static_cast<uint32_t>(*address))
			         .second)
				++duplicated;
			++i;
			auto next = part->GetNextFrameAddress(*address);
			at_end = !next ||
			         next->block_type() != address->block_type() ||
			         next->is_bottom_half_rows() !=
			             address->is_bottom_half_rows() ||
			         next->row() != address->row();
			if (at_end) {
				address.reset();
				break;
			}
			address = next;
		}
		if (i == 0) {
			std::ostringstream err;
			err << "A partial-bitstream block starts at 0x"
			    << std::hex << std::setw(8) << std::setfill('0')
			    << block.first << std::dec
			    << ", which the input .frm does not contain.";
			throw std::runtime_error(err.str());
		}
		const size_t trailing = frames_in_block - i;
		if (at_end) {
			// The last real frame is the last of its row and block
			// type. Anything still in the block goes into the
			// device's dummy frame positions, so it lands nowhere
			// -- but there must be exactly as much of it as was
			// asked for, or the block and the padding rule
			// disagree. One pad is also what flushes the last real
			// frame out of the device's one-frame hold, so
			// --pad_frames 0 leaves such a block unterminated; that
			// is reported by the caller, not refused here, because
			// this arm is also how a row-end block is emitted for
			// the pad-geometry benches.
			if (trailing != pad_frames) {
				std::ostringstream err;
				err << "A partial-bitstream block reaches the "
				       "end of its row with "
				    << trailing
				    << " frame(s) of payload left but "
				       "--pad_frames is "
				    << pad_frames << ".";
				throw std::runtime_error(err.str());
			}
			continue;
		}
		// Mid-row. Exactly one flush frame, at the address the
		// device's counter is about to reach, carrying the static's own
		// content there.
		if (trailing != 1) {
			std::ostringstream err;
			err << "A partial-bitstream block ends in the MIDDLE "
			       "of a row, so its flush frame belongs at 0x"
			    << std::hex << std::setw(8) << std::setfill('0')
			    << (address ? static_cast<uint32_t>(*address) : 0)
			    << std::dec << ", and the block carries "
			    << trailing
			    << " trailing frame(s) there, expected exactly 1. "
			       "The "
			       "device commits N-1 of the N frames in an FDRI "
			       "burst and discards the pending one at the next "
			       "FAR write, so a block with no flush frame "
			       "never writes its own last frame "
			       "(docs/icap-rung4-fdri-and-the-flush-frame.md "
			       "sections 8, 10 and 12; measured on silicon "
			       "2026-09-03 at 71 of 72).";
			throw std::runtime_error(err.str());
		}
		if (!address)
			throw std::runtime_error(
			    "A partial-bitstream block's flush frame has no "
			    "address: the block ends mid-row but the device "
			    "has no next frame.");
		const uint32_t flush_addr = static_cast<uint32_t>(*address);
		auto it = flush_source.find(*address);
		if (it == flush_source.end()) {
			std::ostringstream err;
			err << "A partial-bitstream block's flush frame is at "
			       "0x"
			    << std::hex << std::setw(8) << std::setfill('0')
			    << flush_addr << std::dec
			    << " and the static frame set has no frame there, "
			       "so its content cannot be checked against the "
			       "configuration the device holds.";
			throw std::runtime_error(err.str());
		}
		const size_t off =
		    i * static_cast<size_t>(ArchType::words_per_frame);
		for (size_t w = 0; w < ArchType::words_per_frame; ++w) {
			if (block.second[off + w] == it->second[w])
				continue;
			std::ostringstream err;
			err << "A partial-bitstream block's flush frame at 0x"
			    << std::hex << std::setw(8) << std::setfill('0')
			    << flush_addr << std::dec << " differs from the "
			                                 "static at word "
			    << w << " (0x" << std::hex << block.second[off + w]
			    << " vs 0x" << it->second[w] << std::dec
			    << "). The flush frame must carry the static's own "
			       "configuration at that address: it is outside "
			       "the region, and any other payload -- zeros "
			       "above all -- destroys live static logic if the "
			       "device ever commits it.";
			throw std::runtime_error(err.str());
		}
		if (flush_addresses)
			flush_addresses->insert(flush_addr);
	}

	std::set<uint32_t> wanted;
	for (auto& frame : frames)
		wanted.insert(static_cast<uint32_t>(frame.first));

	if (landed == wanted && duplicated == 0)
		return landed.size();

	std::ostringstream err;
	err << "Partial bitstream frame-set check FAILED. The emitted stream "
	       "would write "
	    << landed.size() << " frame(s); the input .frm has "
	    << wanted.size() << ".";
	if (duplicated)
		err << " " << duplicated << " frame(s) written twice.";
	size_t extra = 0, missing = 0;
	uint32_t first_extra = 0, first_missing = 0;
	for (uint32_t a : landed)
		if (!wanted.count(a)) {
			if (!extra++)
				first_extra = a;
		}
	for (uint32_t a : wanted)
		if (!landed.count(a)) {
			if (!missing++)
				first_missing = a;
		}
	if (extra)
		err << " " << extra
		    << " frame(s) OUTSIDE the input would be written, first 0x"
		    << std::hex << std::setw(8) << std::setfill('0')
		    << first_extra << std::dec
		    << " -- writing a partial bitstream that covers frames the "
		       "caller did not ask for destroys whatever static logic "
		       "lives there.";
	if (missing)
		err << " " << missing
		    << " frame(s) from the input would NOT be written, first "
		       "0x"
		    << std::hex << std::setw(8) << std::setfill('0')
		    << first_missing << std::dec << ".";
	throw std::runtime_error(err.str());
}

template <>
template <typename Collection>
absl::optional<Configuration<Spartan6>>
Configuration<Spartan6>::InitWithPackets(const typename Spartan6::Part& part,
                                         Collection& packets) {
	using ArchType = Spartan6;
	// Registers that can be directly written to.
	uint32_t command_register = 0;
	uint32_t frame_address_register = 0;
	uint32_t mask_register = 0;
	__attribute__((unused)) uint32_t ctl1_register = 0;

	// Internal state machine for writes.
	bool start_new_write = false;
	typename ArchType::FrameAddress current_frame_address = 0;

	Configuration<ArchType>::FrameMap frames;
	for (auto packet : packets) {
		if (packet.opcode() !=
		    ConfigurationPacket<
		        typename ArchType::ConfRegType>::Opcode::Write) {
			continue;
		}

		switch (packet.address()) {
			case ArchType::ConfRegType::MASK:
				if (packet.data().size() < 1)
					continue;
				mask_register = packet.data()[0];
				break;
			case ArchType::ConfRegType::CTL:
				if (packet.data().size() < 1)
					continue;
				ctl1_register =
				    packet.data()[0] & mask_register;
				break;
			case ArchType::ConfRegType::CMD:
				if (packet.data().size() < 1)
					continue;
				command_register = packet.data()[0];
				// Writes to CMD trigger an immediate action. In
				// the case of WCFG, that is just setting a flag
				// for the next FDRI.
				if (command_register == 0x1) {
					start_new_write = true;
				}
				break;
			case ArchType::ConfRegType::IDCODE: {
				// This really should be a two-word write.
				if (packet.data().size() < 2)
					continue;

				// If the IDCODE doesn't match our expected
				// part, consider the bitstream invalid.
				uint32_t idcode = (packet.data()[0] << 16) |
				                  (packet.data()[1]);
				if (idcode != part.idcode()) {
					return {};
				}
				break;
			}
			// UG380 describes the frame addressing scheme where two
			// words for FAR_MAJ update FAR_MAJ anda FAR_MIN -
			// FAR_MAJ comes first
			case ArchType::ConfRegType::FAR_MAJ: {
				size_t packet_size = packet.data().size();
				assert(packet_size < 3);
				if (packet_size < 1) {
					continue;
				} else if (packet_size < 2) {
					frame_address_register =
					    (packet.data()[0] & 0xFFFF) << 16;
				} else {
					frame_address_register =
					    ((packet.data()[0] & 0xFFFF)
					     << 16) |
					    (packet.data()[1] & 0xFFFF);
				}
				break;
			}
			case ArchType::ConfRegType::FAR_MIN:
				// This really should be a one-word write.
				if (packet.data().size() < 1)
					continue;

				frame_address_register |=
				    packet.data()[0] & 0x3FF;

				break;
			case ArchType::ConfRegType::FDRI: {
				if (start_new_write) {
					current_frame_address =
					    frame_address_register;
					start_new_write = false;
				}

				// Spartan6 frames are 65-words long.  Writes
				// to this register can be multiples of that to
				// do auto-incrementing block writes.

				for (size_t ii = 0; ii < packet.data().size();
				     ii += ArchType::words_per_frame) {
					frames[current_frame_address] =
					    packet.data().subspan(
					        ii, ArchType::words_per_frame);

					auto next_address =
					    part.GetNextFrameAddress(
					        current_frame_address);
					if (!next_address)
						break;

					current_frame_address = *next_address;
				}
				break;
			}
			default:
				break;
		}
	}

	return Configuration(part, frames);
}

template <typename ArchType>
template <typename Collection>
absl::optional<Configuration<ArchType>>
Configuration<ArchType>::InitWithPackets(const typename ArchType::Part& part,
                                         Collection& packets) {
	// Registers that can be directly written to.
	uint32_t command_register = 0;
	uint32_t frame_address_register = 0;
	uint32_t mask_register = 0;
	uint32_t ctl1_register = 0;

	// Internal state machine for writes.
	bool start_new_write = false;
	typename ArchType::FrameAddress current_frame_address = 0;

	Configuration<ArchType>::FrameMap frames;
	for (auto packet : packets) {
		if (packet.opcode() !=
		    ConfigurationPacket<
		        typename ArchType::ConfRegType>::Opcode::Write) {
			continue;
		}

		switch (packet.address()) {
			case ArchType::ConfRegType::MASK:
				if (packet.data().size() < 1)
					continue;
				mask_register = packet.data()[0];
				break;
			case ArchType::ConfRegType::CTL1:
				if (packet.data().size() < 1)
					continue;
				ctl1_register =
				    packet.data()[0] & mask_register;
				break;
			case ArchType::ConfRegType::CMD:
				if (packet.data().size() < 1)
					continue;
				command_register = packet.data()[0];
				// Writes to CMD trigger an immediate action. In
				// the case of WCFG, that is just setting a flag
				// for the next FDIR.
				if (command_register == 0x1) {
					start_new_write = true;
				}
				break;
			case ArchType::ConfRegType::IDCODE:
				// This really should be a one-word write.
				if (packet.data().size() < 1)
					continue;

				// If the IDCODE doesn't match our expected
				// part, consider the bitstream invalid.
				if (packet.data()[0] != part.idcode()) {
					return {};
				}
				break;
			case ArchType::ConfRegType::FAR:
				// This really should be a one-word write.
				if (packet.data().size() < 1)
					continue;
				frame_address_register = packet.data()[0];

				// Per UG470, the command present in the CMD
				// register is executed each time the FAR
				// register is laoded with a new value.  As we
				// only care about WCFG commands, just check
				// that here.  CTRL1 is completely undocumented
				// but looking at generated bitstreams, bit 21
				// is used when per-frame CRC is enabled.
				// Setting this bit seems to inhibit the
				// re-execution of CMD during a FAR write.  In
				// practice, this is used so FAR writes can be
				// added in the bitstream to show progress
				// markers without impacting the actual write
				// operation.
				if (bit_field_get(ctl1_register, 21, 21) == 0 &&
				    command_register == 0x1) {
					start_new_write = true;
				}
				break;
			case ArchType::ConfRegType::FDRI: {
				if (start_new_write) {
					current_frame_address =
					    frame_address_register;
					start_new_write = false;
				}

				// Number of words in configuration frames
				// depend on tje architecture.  Writes to this
				// register can be multiples of that number to
				// do auto-incrementing block writes.
				for (size_t ii = 0; ii < packet.data().size();
				     ii += ArchType::words_per_frame) {
					frames[current_frame_address] =
					    packet.data().subspan(
					        ii, ArchType::words_per_frame);

					auto next_address =
					    part.GetNextFrameAddress(
					        current_frame_address);
					if (!next_address)
						break;

					// Bitstreams appear to have 2 frames of
					// padding between rows.
					if (next_address &&
					    (next_address->block_type() !=
					         current_frame_address
					             .block_type() ||
					     next_address
					             ->is_bottom_half_rows() !=
					         current_frame_address
					             .is_bottom_half_rows() ||
					     next_address->row() !=
					         current_frame_address.row())) {
						ii += 2 *
						      ArchType::words_per_frame;
					}
					current_frame_address = *next_address;
				}
				break;
			}
			default:
				break;
		}
	}

	return Configuration(part, frames);
}

template <typename ArchType>
void Configuration<ArchType>::PrintFrameAddresses(FILE* fp) {
	fprintf(fp, "Frame addresses in bitstream: ");
	for (auto frame = frames_.begin(); frame != frames_.end(); ++frame) {
		fprintf(fp, "%08X", (int)frame->first);
		if (std::next(frame) != frames_.end()) {
			fprintf(fp, " ");
		} else {
			fprintf(fp, "\n");
		}
	}
}

}  // namespace xilinx
}  // namespace prjxray

#endif  // PRJXRAY_LIB_XILINX_CONFIGURATION_H_
