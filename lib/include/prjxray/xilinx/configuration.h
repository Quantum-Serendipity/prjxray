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

namespace prjxray {
namespace xilinx {

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
	static void createConfigurationPackage(
	    typename ArchType::ConfigurationPackage& out_packets,
	    const PacketData& packet_data,
	    absl::optional<typename ArchType::Part>& part);

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

	// Splits `frames` into those runs. `pad_frames` zero frames are
	// appended to a block ONLY when its last frame is the last frame of
	// its row and block type -- the one place the device's frame-address
	// counter has dummy positions, so the pads land nowhere. A block that
	// stops in the middle of a row never gets them, because there the pads
	// would land on live static frames.
	//
	// That rule is measured, not assumed. In the Vivado 2025.2 golden
	// partial for xc7a100tcsg324-1 (gateware/vivado-dfx-golden/):
	//   CLB  block  83325 words = 825 frames, ending at row 0 col 56
	//                minor 0 -- MID-ROW, and it carries NO pad;
	//   BRAM block  38986 words = 386 frames = 384 real frames ending at
	//                row 0 col 3 minor 127 -- a ROW END -- PLUS exactly
	//                two all-zero frames.
	// So --pad_frames 0 emits neither and --pad_frames 2 reproduces
	// Vivado's stream. 0 is the default: it is the value for which no
	// frame outside the input can be written under any input.
	//
	// Throws std::runtime_error if any input address is not a valid frame
	// address on `part`.
	static PartialPacketData createPartialPacketData(
	    const typename Frames<ArchType>::Frames2Data& frames,
	    absl::optional<typename ArchType::Part>& part,
	    unsigned int pad_frames);

	// Re-derives the frame set the emitted blocks will land on -- by
	// walking GetNextFrameAddress FORWARD from each block's FAR, which is
	// the device's own rule and a different code path from the grouping
	// above -- and throws unless it is exactly `frames`. Returns the count.
	//
	// This is Phase 6 Unit 6.3 step 5: `fasm2frames --roi` is a union
	// rather than a filter, so an ROI leak is silent, and the deferred half
	// of that fix was to make the writer check the frame set on EVERY
	// emission rather than only when someone passes a flag. Hence: called
	// unconditionally, no flag to forget.
	static size_t checkPartialFrameSet(
	    const PartialPacketData& blocks,
	    const typename Frames<ArchType>::Frames2Data& frames,
	    absl::optional<typename ArchType::Part>& part,
	    unsigned int pad_frames);

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
	static void createPartialConfigurationPackage(
	    typename ArchType::ConfigurationPackage& out_packets,
	    const PartialPacketData& cfg_packets,
	    absl::optional<typename ArchType::Part>& part);

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
    absl::optional<Spartan6::Part>& part);

template <>
void Configuration<Series7>::createConfigurationPackage(
    Series7::ConfigurationPackage& out_packets,
    const PacketData& packet_data,
    absl::optional<Series7::Part>& part);

template <>
void Configuration<UltraScale>::createConfigurationPackage(
    UltraScale::ConfigurationPackage& out_packets,
    const PacketData& packet_data,
    absl::optional<UltraScale::Part>& part);

template <>
void Configuration<UltraScalePlus>::createConfigurationPackage(
    UltraScalePlus::ConfigurationPackage& out_packets,
    const PacketData& packet_data,
    absl::optional<UltraScalePlus::Part>& part);

// Same reason as above: these four are defined in configuration.cc and must be
// declared before use in every translation unit.
template <>
void Configuration<Spartan6>::createPartialConfigurationPackage(
    Spartan6::ConfigurationPackage& out_packets,
    const PartialPacketData& cfg_packets,
    absl::optional<Spartan6::Part>& part);

template <>
void Configuration<Series7>::createPartialConfigurationPackage(
    Series7::ConfigurationPackage& out_packets,
    const PartialPacketData& cfg_packets,
    absl::optional<Series7::Part>& part);

template <>
void Configuration<UltraScale>::createPartialConfigurationPackage(
    UltraScale::ConfigurationPackage& out_packets,
    const PartialPacketData& cfg_packets,
    absl::optional<UltraScale::Part>& part);

template <>
void Configuration<UltraScalePlus>::createPartialConfigurationPackage(
    UltraScalePlus::ConfigurationPackage& out_packets,
    const PartialPacketData& cfg_packets,
    absl::optional<UltraScalePlus::Part>& part);

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
			// `boundary` still describes the PREVIOUS frame here:
			// pad the block we are closing only if it ended on a
			// row/bus boundary.
			if (!blocks.empty() && boundary)
				blocks.back().second.insert(
				    blocks.back().second.end(), pad_words, 0);
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
	if (!blocks.empty() && boundary)
		blocks.back().second.insert(blocks.back().second.end(),
		                            pad_words, 0);
	return blocks;
}

template <typename ArchType>
size_t Configuration<ArchType>::checkPartialFrameSet(
    const PartialPacketData& blocks,
    const typename Frames<ArchType>::Frames2Data& frames,
    absl::optional<typename ArchType::Part>& part,
    unsigned int pad_frames) {
	std::set<uint32_t> landed;
	size_t duplicated = 0;
	for (auto& block : blocks) {
		const size_t frames_in_block =
		    block.second.size() / ArchType::words_per_frame;
		absl::optional<typename ArchType::FrameAddress> address(
		    typename ArchType::FrameAddress(block.first));
		size_t i = 0;
		while (i < frames_in_block) {
			if (!address)
				throw std::runtime_error(
				    "A partial-bitstream block runs past the "
				    "last frame of the device.");
			if (!landed.insert(static_cast<uint32_t>(*address))
			         .second)
				++duplicated;
			++i;
			auto next = part->GetNextFrameAddress(*address);
			const bool at_end =
			    !next ||
			    next->block_type() != address->block_type() ||
			    next->is_bottom_half_rows() !=
			        address->is_bottom_half_rows() ||
			    next->row() != address->row();
			if (at_end) {
				// The frame just written is the last of its row
				// and block type. Anything still in the block
				// goes into the device's dummy frame
				// positions, so it lands nowhere -- but there
				// must be exactly as much of it as was asked
				// for, or the block and the padding rule
				// disagree.
				if (frames_in_block - i != pad_frames) {
					std::ostringstream err;
					err << "A partial-bitstream block "
					       "reaches the end of its row "
					       "with "
					    << (frames_in_block - i)
					    << " frame(s) of payload left but "
					       "--pad_frames is "
					    << pad_frames << ".";
					throw std::runtime_error(err.str());
				}
				break;
			}
			address = next;
		}
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
