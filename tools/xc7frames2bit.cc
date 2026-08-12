/*
 * Copyright (C) 2017-2020  The Project X-Ray Authors.
 *
 * Use of this source code is governed by a ISC-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/ISC
 *
 * SPDX-License-Identifier: ISC
 */
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <gflags/gflags.h>
#include <prjxray/xilinx/architectures.h>
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

namespace xilinx = prjxray::xilinx;

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
			typename ArchType::ConfigurationPackage
			    configuration_package;
			try {
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

				std::cerr << "xc7frames2bit: partial "
				             "bitstream, "
				          << emitted << " frames in "
				          << cfg_packets.size()
				          << " FAR block(s)" << std::endl;

				// If the partial bitstream feature is not
				// supported for a target architecture we
				// should catch an exception about it here
				xilinx::Configuration<ArchType>::
				    createPartialConfigurationPackage(
				        configuration_package, cfg_packets,
				        part);
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
			        FLAGS_output_file)) {
				std::cerr << "Failed to write bitstream"
				          << std::endl
				          << "Exitting" << std::endl;
				return 1;
			}
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
