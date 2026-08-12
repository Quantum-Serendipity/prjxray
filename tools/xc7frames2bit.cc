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
#include <sstream>
#include <stdexcept>
#include <vector>

#include <gflags/gflags.h>
#include <prjxray/xilinx/architectures.h>
#include <prjxray/xilinx/bitstream_reader.h>
#include <prjxray/xilinx/bitstream_writer.h>
#include <prjxray/xilinx/configuration.h>

DEFINE_string(part_name, "", "Name of the 7-series part");
DEFINE_string(part_file, "", "Definition file for target 7-series part");
DEFINE_string(
    frm_file,
    "",
    "File containing a list of frame deltas to be applied to the base "
    "bitstream.  Each line in the file is of the form: "
    "<frame_address> <word1>,...,<word101>.");
DEFINE_string(output_file, "", "Write bitstream to file");
DEFINE_string(architecture,
              "Series7",
              "Architecture of the provided bitstream");
DEFINE_bool(partial_bitstream,
            false,
            "Generate a partial bitstream. The frame set is DERIVED FROM THE "
            "INPUT: exactly the frames listed in --frm_file are written, and "
            "nothing else. Use fasm2frames --sparse --roi <roi.json> "
            "--roi-strict to produce that .frm, and note that plain --roi is "
            "a union rather than a filter.");
DEFINE_uint32(pad_frames,
              0,
              "Zero frames appended to a FAR block of a partial bitstream that "
              "ends at a row or block-type boundary -- the one place the "
              "device's frame-address counter has dummy positions, so the pads "
              "land nowhere. A block that stops mid-row never gets them, "
              "because there a pad would land on a live static frame. Vivado "
              "follows the same rule: in the measured golden its CLB block "
              "ends mid-row and carries no pad, and its BRAM block ends at a "
              "row end and carries exactly two. 0 is the default because it is "
              "the value for which no frame outside the input can be written "
              "under any input; 2 reproduces Vivado's stream.");
DEFINE_uint32(expected_frames,
              0,
              "If non-zero, fail unless the partial bitstream writes exactly "
              "this many frames. This is the ROI's own frame count (1004 for "
              "the two-clock-region reference ROI on xc7a100t); a materially "
              "larger number means the fasm2frames --roi union has leaked.");
DEFINE_string(
    static_frames,
    "",
    "A .frm carrying the LOCKED STATIC DESIGN's own content over the region's "
    "frames. Required by the FAR double-write pre-pass and NOT recoverable "
    "from --frm_file: that file is already STATIC | RM, which is pass 2, and "
    "the union has destroyed each bit's provenance. A DFX flow holds this by "
    "construction -- it is the same object it merged to build pass 2. Produce "
    "it with `bitread -o` + bitread-to-frm from the locked static's bitstream, "
    "or with fasm2frames --roi-strict over the static's FASM.");
DEFINE_string(
    prepass,
    "auto",
    "FAR double-write pre-pass (docs/far-double-write-verdict.md section 5.1). "
    "auto: emit it when --static_frames is given, otherwise emit a single pass "
    "and WARN. on: emit it, and REFUSE if --static_frames is missing. off: "
    "single pass, silently -- the pre-Unit-6.2 stream. nostatic: emit the "
    "pre-pass with an empty static and warn; the result is wrong in exactly "
    "the bits carrying the static's own routing inside the region.");
DEFINE_bool(raw,
            false,
            "Write a raw configuration word stream for the ICAP instead of a "
            ".bit: no .bit TLV header, and no IDCODE register write. A .bit "
            "loaded over JTAG carries IDCODE; a raw ICAP stream does not.");
DEFINE_string(crc,
              "compute",
              "compute: compute the stream CRC-32C and write it to the CRC "
              "register. disable: substitute a CMD:RCRC write, which is "
              "exactly what Vivado's BITSTREAM.GENERAL.CRC=DISABLE emits -- a "
              "two-word substitution, not a second code path.");
DEFINE_bool(verify,
            true,
            "Read the emitted bitstream back through the bitstream READER and "
            "check that every frame of --frm_file landed at its address with "
            "its exact payload, ECC included. On by default: it costs one "
            "in-process parse, and a verifier that must be requested will not "
            "be. --noverify opts out.");
DEFINE_uint32(trailing_nops,
              16,
              "NOP words after CMD:DESYNC. 16 in every Vivado golden partial.");

namespace xilinx = prjxray::xilinx;

namespace {

// Read the file we just wrote, through the bitstream READER -- a different code
// path from the writer, which is the whole point -- and check that every frame
// of the input landed at its address with its exact payload.
//
// For a double-written stream the reader's frame map keeps the LAST write of
// each address, which is pass 2, which is the configuration. So this verifies
// the pass that matters and is blind to the pre-pass by construction; the
// pre-pass is checked structurally at build time instead, because a frame-level
// comparator is definitionally blind to it.
template <typename ArchType>
int VerifyRoundTrip(const std::string& path,
                    const typename ArchType::Part& part,
                    const typename xilinx::Frames<ArchType>::Frames2Data& want) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		std::cerr << "--verify: cannot reopen " << path << std::endl;
		return 1;
	}
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
	                           std::istreambuf_iterator<char>());
	absl::Span<uint8_t> span(bytes.data(), bytes.size());
	auto reader = xilinx::BitstreamReader<ArchType>::InitWithBytes(span);
	if (!reader) {
		std::cerr << "--verify: " << path
		          << " has no sync word; it is not a bitstream."
		          << std::endl;
		return 1;
	}
	auto config =
	    xilinx::Configuration<ArchType>::InitWithPackets(part, *reader);
	if (!config) {
		std::cerr << "--verify: " << path
		          << " does not decode as a bitstream for this part."
		          << std::endl;
		return 1;
	}
	const auto& got = config->frames();
	size_t missing = 0, wrong = 0, extra = 0;
	uint32_t first_missing = 0, first_wrong = 0, first_extra = 0;
	size_t first_wrong_word = 0;
	for (const auto& frame : want) {
		auto it = got.find(frame.first);
		if (it == got.end()) {
			if (!missing++)
				first_missing =
				    static_cast<uint32_t>(frame.first);
			continue;
		}
		if (it->second.size() != frame.second.size()) {
			if (!wrong++)
				first_wrong = static_cast<uint32_t>(frame.first);
			continue;
		}
		for (size_t w = 0; w < frame.second.size(); ++w) {
			if (it->second[w] != frame.second[w]) {
				if (!wrong++) {
					first_wrong =
					    static_cast<uint32_t>(frame.first);
					first_wrong_word = w;
				}
				break;
			}
		}
	}
	for (const auto& frame : got) {
		if (want.find(frame.first) == want.end()) {
			if (!extra++)
				first_extra = static_cast<uint32_t>(frame.first);
		}
	}
	if (!missing && !wrong && !extra) {
		std::cerr << "xc7frames2bit: --verify round-trip OK, "
		          << want.size() << " frames, ECC included" << std::endl;
		return 0;
	}
	std::cerr << "xc7frames2bit: --verify FAILED. " << want.size()
	          << " frame(s) in, " << got.size() << " read back;";
	if (missing)
		std::cerr << " " << missing << " missing, first 0x" << std::hex
		          << std::setw(8) << std::setfill('0') << first_missing
		          << std::dec << ";";
	if (wrong)
		std::cerr << " " << wrong << " with a different payload, first "
		          << "0x" << std::hex << std::setw(8)
		          << std::setfill('0') << first_wrong << std::dec
		          << " at word " << first_wrong_word << ";";
	if (extra)
		std::cerr << " " << extra
		          << " frame(s) written that the input did not ask "
		             "for, first 0x"
		          << std::hex << std::setw(8) << std::setfill('0')
		          << first_extra << std::dec << ";";
	std::cerr << std::endl;
	return 1;
}

}  // namespace

struct Frames2BitWriter {
	template <typename T>
	int operator()(T& arg) {
		using ArchType = std::decay_t<decltype(arg)>;
		auto part = ArchType::Part::FromFile(FLAGS_part_file);
		if (!part) {
			std::cerr << "Part file " << FLAGS_part_file
			          << " not found or invalid" << std::endl;
			return 1;
		}

		// Read the frames from the input file
		xilinx::Frames<ArchType> frames;
		if (frames.readFrames(FLAGS_frm_file)) {
			std::cerr << "Frames file " << FLAGS_frm_file
			          << " not found or invalid" << std::endl;
			return 1;
		}

		if (FLAGS_partial_bitstream) {
			// DERIVE-FROM-INPUT.
			//
			// The frame set of a partial bitstream is the frame set
			// of the .frm, exactly. There is no address range to
			// declare and nothing to fill in, so there is no way to
			// write a frame the caller did not ask for.
			//
			// The design this replaces declared the region as two
			// numeric address ranges and zero-filled every device
			// frame inside them that the .frm did not mention. An
			// ROI is a rectangle in (row, column); an address range
			// is an interval; the two are different sets. Measured
			// on xc7a100tcsg324-1 against the 1,004-frame ROI
			// GRID_X 74..101 / GRID_Y 1..103: 3,040 frames emitted
			// (32.2% of the part for a region that is 10.6% of it),
			// 2,032 of them outside the ROI, 2,031 zero-filled --
			// 801.3 KiB of live static logic destroyed, at exit 0
			// with no warning.
			//
			// cfg_packets is declared HERE, not inside the try
			// block and not inside the callee, because the FDRI
			// packets hold non-owning absl::Spans over its vectors
			// and BitstreamWriter reads them below. Same lifetime
			// rule, same failure mode, as the bitread span fix.
			typename xilinx::Configuration<ArchType>::
			    PartialPacketData cfg_packets;
			// Pass 1's frames and blocks are declared HERE for the
			// same lifetime reason as cfg_packets: the emitted FDRI
			// packets hold non-owning spans over them.
			typename xilinx::Frames<ArchType>::Frames2Data
			    prepass_frames;
			typename xilinx::Configuration<ArchType>::
			    PartialPacketData all_blocks;
			typename ArchType::ConfigurationPackage
			    configuration_package;
			xilinx::PartialStreamOptions options;
			options.emit_idcode = !FLAGS_raw;
			options.trailing_nops = FLAGS_trailing_nops;
			try {
				if (FLAGS_crc == "compute")
					options.compute_crc = true;
				else if (FLAGS_crc == "disable")
					options.compute_crc = false;
				else
					throw std::runtime_error(
					    "--crc must be 'compute' or "
					    "'disable'.");

				const bool have_static =
				    !FLAGS_static_frames.empty();
				bool want_prepass;
				bool use_static = have_static;
				if (FLAGS_prepass == "off") {
					want_prepass = false;
				} else if (FLAGS_prepass == "auto") {
					want_prepass = have_static;
				} else if (FLAGS_prepass == "on") {
					if (!have_static)
						throw std::runtime_error(
						    "--prepass on needs "
						    "--static_frames: the "
						    "pre-pass carries the "
						    "static design's own "
						    "routing inside the "
						    "region, and that is not "
						    "recoverable from "
						    "--frm_file, which is "
						    "already STATIC | RM. "
						    "Pass the locked static's "
						    "frames, or use --prepass "
						    "nostatic to accept a "
						    "degraded pre-pass "
						    "knowingly.");
					want_prepass = true;
				} else if (FLAGS_prepass == "nostatic") {
					want_prepass = true;
					use_static = false;
				} else {
					throw std::runtime_error(
					    "--prepass must be auto, on, off "
					    "or nostatic.");
				}

				if (frames.getFrames().empty())
					throw std::runtime_error(
					    "Refusing to write a partial "
					    "bitstream from an empty frames "
					    "file: there would be nothing to "
					    "configure.");

				cfg_packets = xilinx::Configuration<ArchType>::
				    createPartialPacketData(frames.getFrames(),
				                            part,
				                            FLAGS_pad_frames);

				// Unconditional, per Unit 6.3 step 5: the
				// check runs on every emission rather than
				// behind a flag someone has to remember.
				size_t emitted =
				    xilinx::Configuration<ArchType>::
				        checkPartialFrameSet(
				            cfg_packets, frames.getFrames(),
				            part, FLAGS_pad_frames);

				if (FLAGS_expected_frames &&
				    emitted != FLAGS_expected_frames) {
					std::ostringstream err;
					err << "Partial bitstream writes "
					    << emitted
					    << " frames but --expected_frames "
					       "is "
					    << FLAGS_expected_frames
					    << ". A larger number than the "
					       "ROI's own frame count means "
					       "the fasm2frames --roi union "
					       "has leaked; use --roi-strict.";
					throw std::runtime_error(err.str());
				}

				if (want_prepass) {
					xilinx::Frames<ArchType> static_frames;
					if (use_static &&
					    static_frames.readFrames(
					        FLAGS_static_frames)) {
						throw std::runtime_error(
						    "Static frames file " +
						    FLAGS_static_frames +
						    " not found or invalid");
					}
					if (!use_static) {
						std::cerr
						    << "xc7frames2bit: WARNING: "
						       "emitting the FAR "
						       "double-write pre-pass "
						       "with NO static design. "
						       "Pass 1 will be missing "
						       "the static's own "
						       "routing inside the "
						       "region -- interface-net "
						       "routing-mux selectors "
						       "in INT tiles at the "
						       "region's edge, at "
						       "minors 2-15 (84 bits in "
						       "67 words in the "
						       "measured golden). That "
						       "is wrong exactly where "
						       "a partial "
						       "reconfiguration can "
						       "damage the static it "
						       "exists to preserve. Use "
						       "--static_frames."
						    << std::endl;
					}
					prepass_frames =
					    xilinx::Configuration<ArchType>::
					        createPrePassFrames(
					            frames.getFrames(),
					            static_frames.getFrames(),
					            use_static);
					auto prepass_blocks =
					    xilinx::Configuration<ArchType>::
					        createPartialPacketData(
					            prepass_frames, part,
					            FLAGS_pad_frames);
					// Both passes must land on exactly the
					// same frames. Checking pass 1's frame
					// set separately is what stops a
					// pre-pass from writing outside the
					// region.
					size_t emitted1 =
					    xilinx::Configuration<ArchType>::
					        checkPartialFrameSet(
					            prepass_blocks,
					            prepass_frames, part,
					            FLAGS_pad_frames);
					if (emitted1 != emitted ||
					    prepass_blocks.size() !=
					        cfg_packets.size())
						throw std::runtime_error(
						    "The pre-pass and the "
						    "configuration pass do not "
						    "cover the same frames.");
					for (size_t i = 0;
					     i < prepass_blocks.size(); ++i) {
						if (prepass_blocks[i].first !=
						        cfg_packets[i].first ||
						    prepass_blocks[i]
						            .second.size() !=
						        cfg_packets[i]
						            .second.size())
							throw std::runtime_error(
							    "The pre-pass and "
							    "the configuration "
							    "pass disagree "
							    "about a FAR "
							    "block.");
					}
					// Pass 1 of EVERY block, then pass 2 of
					// every block: A B A B, which is
					// Vivado's order, not A A B B.
					all_blocks = prepass_blocks;
					all_blocks.insert(all_blocks.end(),
					                  cfg_packets.begin(),
					                  cfg_packets.end());
				} else {
					all_blocks = cfg_packets;
					if (FLAGS_prepass != "off")
						std::cerr
						    << "xc7frames2bit: WARNING: "
						       "no --static_frames, so "
						       "NO FAR double-write "
						       "pre-pass is emitted. "
						       "Vivado writes every FAR "
						       "block twice and this "
						       "stream writes each once "
						       "-- structurally "
						       "incomplete against the "
						       "vendor, and a "
						       "frame-level comparator "
						       "cannot see the "
						       "difference. Pass "
						       "--static_frames, or "
						       "--prepass off to say "
						       "you meant it."
						    << std::endl;
				}

				std::cerr << "xc7frames2bit: partial "
				             "bitstream, "
				          << emitted << " frames in "
				          << cfg_packets.size()
				          << " FAR block(s), "
				          << (want_prepass ? 2 : 1)
				          << " pass(es), "
				          << all_blocks.size()
				          << " FDRI block(s) emitted"
				          << std::endl;

				// If the partial bitstream feature is not
				// supported for a target architecture we
				// should catch an exception about it here
				xilinx::Configuration<ArchType>::
				    createPartialConfigurationPackage(
				        configuration_package, all_blocks, part,
				        options);
			} catch (const std::runtime_error& err) {
				std::cerr << err.what() << std::endl;
				return 1;
			}

			// Write bitstream
			auto bitstream_writer =
			    xilinx::BitstreamWriter<ArchType>(
			        configuration_package);
			if (bitstream_writer.writeBitstream(
			        configuration_package, FLAGS_part_name,
			        FLAGS_frm_file, "xc7frames2bit",
			        FLAGS_output_file, FLAGS_raw)) {
				std::cerr << "Failed to write bitstream"
				          << std::endl
				          << "Exitting" << std::endl;
				return 1;
			}

			if (FLAGS_verify &&
			    VerifyRoundTrip<ArchType>(FLAGS_output_file, *part,
			                              frames.getFrames()))
				return 1;
		} else {
			if (std::is_same<ArchType, xilinx::Series7>::value ||
			    std::is_same<ArchType, xilinx::UltraScale>::value ||
			    std::is_same<ArchType,
			                 xilinx::UltraScalePlus>::value) {
				// In case the frames input file is missing some
				// frames that are in the tilegrid
				frames.addMissingFrames(part);
			}
			// Create data for the type 2 configuration packet with
			// information about all frames
			typename xilinx::Configuration<ArchType>::PacketData
			    configuration_packet_data(
			        xilinx::Configuration<ArchType>::
			            createType2ConfigurationPacketData(
			                frames.getFrames(), part));

			// Put together a configuration package
			typename ArchType::ConfigurationPackage
			    configuration_package;
			xilinx::Configuration<ArchType>::
			    createConfigurationPackage(
			        configuration_package,
			        configuration_packet_data, part);

			// Write bitstream
			auto bitstream_writer =
			    xilinx::BitstreamWriter<ArchType>(
			        configuration_package);
			if (bitstream_writer.writeBitstream(
			        configuration_package, FLAGS_part_name,
			        FLAGS_frm_file, "xc7frames2bit",
			        FLAGS_output_file)) {
				std::cerr << "Failed to write bitstream"
				          << std::endl
				          << "Exitting" << std::endl;
				// It printed "Failed" and then returned 0.
				return 1;
			}

			if (FLAGS_verify &&
			    VerifyRoundTrip<ArchType>(FLAGS_output_file, *part,
			                              frames.getFrames()))
				return 1;
		}

		return 0;
	}
};

int main(int argc, char* argv[]) {
	gflags::SetUsageMessage(argv[0]);
	gflags::ParseCommandLineFlags(&argc, &argv, true);
	xilinx::Architecture::Container arch_container =
	    xilinx::ArchitectureFactory::create_architecture(
	        FLAGS_architecture);
	return absl::visit(Frames2BitWriter(), arch_container);
}
