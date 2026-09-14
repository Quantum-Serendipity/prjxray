/*
 * Copyright (C) 2017-2020  The Project X-Ray Authors.
 *
 * Use of this source code is governed by a ISC-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/ISC
 *
 * SPDX-License-Identifier: ISC
 */
#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <gflags/gflags.h>
#include <prjxray/xilinx/architectures.h>
#include <prjxray/xilinx/bitstream_reader.h>
#include <prjxray/xilinx/bitstream_writer.h>
#include <prjxray/xilinx/configuration.h>
#include <prjxray/xilinx/xc7series/configuration_options_1_value.h>

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
              "ROW-END PAD SLOTS: zero frames appended to a FAR block of a "
              "partial bitstream that ends at a row or block-type boundary -- "
              "the one place the device's frame-address counter has dummy "
              "positions, so they land nowhere. A block that stops mid-row "
              "never gets them, because there a pad would land on a live "
              "static frame; it gets one FLUSH FRAME instead, which is a "
              "different object, is not optional, and this flag does not "
              "govern it. Vivado's measured golden: its BRAM block ends at a "
              "row end and carries two pad slots, and its CLB block ends "
              "mid-row and carries one flush frame at 0x00001C00, column 56, "
              "OUTSIDE the pblock -- an earlier version of this text read that "
              "frame as region content and concluded 'no pad', which is how a "
              "stream that never commits its own last frame passed every "
              "check this tool had. 2 reproduces Vivado's pad slots; 0 is the "
              "default and leaves a row-end block flushed by nothing.");
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
    "frames, PLUS ONE FRAME PAST THE LAST FRAME OF EACH FAR BLOCK. Two "
    "consumers. (1) The FAR double-write pre-pass, which needs the static term "
    "over the region and cannot recover it from --frm_file: that file is "
    "already STATIC | RM, which is pass 2, and the union has destroyed each "
    "bit's provenance. (2) The FLUSH FRAME of any block that ends mid-row -- "
    "the device commits N-1 of an FDRI burst's N frames, so such a block needs "
    "one frame past its last, and that frame is written at a real address "
    "OUTSIDE the region. Its content is the static's own configuration there, "
    "which is safe whether or not the device commits it; there is no zero "
    "fallback, because a zero frame that did commit would erase live static "
    "logic. A DFX flow holds both by construction. Produce it with `bitread "
    "-o` + bitread-to-frm from the locked static's bitstream, or with "
    "fasm2frames --roi-strict over the static's FASM plus the trailing "
    "address(es) taken from the static's FULL frame set.");
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
              "PARTIAL BITSTREAMS ONLY (--partial_bitstream). compute: "
              "compute the stream CRC-32C and write it to the CRC register. "
              "disable: substitute a CMD:RCRC write, which is exactly what "
              "Vivado's BITSTREAM.GENERAL.CRC=DISABLE emits -- a two-word "
              "substitution, not a second code path. The FULL path's switch "
              "is --emit_crc, and it is a separate flag on purpose: this one "
              "defaults to compute.");
DEFINE_bool(emit_crc,
            false,
            "FULL BITSTREAMS ONLY. Write the stream CRC-32C into the CRC "
            "register at the two positions a Vivado full bitstream uses -- "
            "after the frame data and before CMD:DESYNC -- instead of the two "
            "CMD:RCRC writes this tool has always emitted there. Those two "
            "CMD:RCRC writes ARE Vivado's CRC=DISABLE substitution, so every "
            "full bitstream from this tool is a CRC-DISABLED bitstream and "
            "the device's own corruption check never runs on it: measured on "
            "silicon, a single flipped configuration bit in a 3.8 MB stream "
            "loads to DONE=1 with 'No CRC error' and runs 39x wrong "
            "(docs/h13-hardware-negative-controls.md). OFF BY DEFAULT because "
            "turning it on changes the payload of every full bitstream this "
            "toolchain builds, and hundreds of recorded payload hashes "
            "identify artefacts by exactly that payload.");
DEFINE_string(
    rbcrc,
    "off",
    "FULL BITSTREAMS ONLY, Series7 only. COR1[8] RBCRC_EN plus COR1[16:15] "
    "RBCRC_ACTION -- the device's own continuous background readback of "
    "configuration memory (UG470 ch. 8), which is what detects a "
    "single-event upset in a region that is running and never reloaded. "
    "off (default): COR1 = 0x00000000 and no COR1 write moves, which is what "
    "every full bitstream this writer has ever emitted carries and why the "
    "feature has been unreachable from this flow. continue | halt | "
    "correct_and_halt | correct_and_continue: RBCRC_EN set and RBCRC_ACTION "
    "encoded per Table 5-33, whose encoding is 00/01/11/10 and is NOT in "
    "menu order. Enabling it changes exactly one word of the emitted stream. "
    "TWO THINGS IT DOES NOT DO. It is not an identity check: UG470 ch. 8 says "
    "'the hardware golden CRC is automatically regenerated after any write to "
    "FDRI', so the scrubber adopts whatever a partial reconfiguration leaves "
    "and cannot tell an authorised load from corruption. And it needs a "
    "clock: per Table 8-1 the engine runs off the ICAPE2 CLK, or STARTUPE2's "
    "USRCCLKO, or an internal oscillator selected by POST_CRC_FREQ -- and a "
    "JTAG-configured design instantiating neither primitive gets NO CLOCK, "
    "so the bit is set and the scan never runs.");
DEFINE_bool(rbcrc_error_pin,
            true,
            "COR1[9] RBCRC_NO_PIN, INVERTED to match the pin's behaviour "
            "rather than the bit's name: true (the default, and the "
            "register's default) leaves INIT_B driven Low on a readback-CRC "
            "mismatch; false sets RBCRC_NO_PIN and frees the pin. Only "
            "meaningful with --rbcrc, and passing it alone is a refusal, not "
            "a no-op.");
DEFINE_bool(compress,
            false,
            "FULL BITSTREAMS ONLY, Series7 only. Encode the frame data with "
            "the configuration engine's own MULTIPLE FRAME WRITE instead of "
            "one type-2 FDRI burst over the whole device. A run of identical "
            "frames is written once by FDRI and then replayed to each further "
            "address by a FAR write plus a 4-word MFWR packet -- 7 words per "
            "frame against 101 -- which is what Vivado's "
            "BITSTREAM.GENERAL.COMPRESS emits and what this flow has never "
            "had. The device decompresses it in the configuration engine, "
            "before any fabric exists, so unlike a fabric decompressor it "
            "applies to the STATIC bitstream that boots from flash. The "
            "saving is a property of the design's frame density, not of the "
            "flag: it comes from all-zero frames, which are most of a "
            "7-series device. --verify covers it end to end -- the emitted "
            "stream is read back through the bitstream READER, whose MFWR "
            "case expands the replays, and every frame must match the input "
            "word for word. NOT VALIDATED ON SILICON: offline expansion "
            "equality is not proof the engine accepts the stream.");
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
DEFINE_string(
    db_root,
    "",
    "prjxray-db FAMILY directory -- the one holding segbits_*.db and "
    "mapping/parts.yaml, e.g. <prjxray-db>/artix7. The FAR double-write "
    "pre-pass needs it: its SITE mask is per TILE, so it is a function of "
    "tilegrid.json's tile lattice and of prjxray's own segbits, not of "
    "(minor, word). Defaults to the grandparent of --part_file, which is that "
    "directory whenever part.yaml is read from where prjxray-db puts it; "
    "failing that, $XRAY_DATABASE_DIR/$XRAY_DATABASE.");
DEFINE_string(tilegrid,
              "",
              "Override the tilegrid.json the pre-pass reads. By default it is "
              "resolved from --part_name through --db_root's own "
              "mapping/parts.yaml and mapping/devices.yaml, because the PART "
              "directory (xc7a100tcsg324-1) and the FABRIC directory "
              "(xc7a100t) are different directories.");

namespace xilinx = prjxray::xilinx;
namespace xc7series = prjxray::xilinx::xc7series;

namespace {

// --rbcrc's spelling -> COR1[16:15]. Returns false on an unknown name; "off"
// is handled by the caller, because it is the absence of a COR1 override
// rather than a value of one.
bool ParseRbcrcAction(
    const std::string& name,
    xc7series::ConfigurationOptions1Value::ReadbackCrcAction* out) {
	using Action = xc7series::ConfigurationOptions1Value::ReadbackCrcAction;
	if (name == "continue")
		*out = Action::Continue;
	else if (name == "halt")
		*out = Action::Halt;
	else if (name == "correct_and_halt")
		*out = Action::CorrectAndHalt;
	else if (name == "correct_and_continue")
		*out = Action::CorrectAndContinue;
	else
		return false;
	return true;
}

// The pre-pass geometry, from wherever the caller's flags point. There is NO
// fallback: a mask that cannot be built from the database is not built at all,
// because the thing it would fall back to -- SITE as a function of (minor,
// word) -- is the defect this replaces, and it is wrong precisely in the
// columns nobody looks at.
std::unique_ptr<xc7series::PrePassGeometry> LoadPrePassGeometry(
    std::string* error) {
	std::string root = FLAGS_db_root;
	if (root.empty() && !FLAGS_part_file.empty()) {
		// <db>/<family>/<part>/part.yaml -> <db>/<family>
		size_t part_dir = FLAGS_part_file.find_last_of('/');
		if (part_dir != std::string::npos && part_dir > 0) {
			size_t family =
			    FLAGS_part_file.find_last_of('/', part_dir - 1);
			if (family != std::string::npos)
				root = FLAGS_part_file.substr(0, family);
		}
	}
	if (root.empty()) {
		const char* dir = getenv("XRAY_DATABASE_DIR");
		const char* db = getenv("XRAY_DATABASE");
		if (dir && db)
			root = std::string(dir) + "/" + db;
	}
	if (root.empty()) {
		*error =
		    "the FAR double-write pre-pass needs prjxray's database "
		    "and none of --db_root, --part_file or "
		    "$XRAY_DATABASE_DIR/$XRAY_DATABASE says where it is.";
		return nullptr;
	}
	if (!FLAGS_tilegrid.empty())
		return xc7series::PrePassGeometry::Load(FLAGS_tilegrid, root,
		                                        error);
	return xc7series::PrePassGeometry::LoadForPart(root, FLAGS_part_name,
	                                               error);
}

// What the pre-pass did, on stderr. The two "inferred" lines exist because
// docs/far-double-write-verdict.md section 5.1 flags two elements of the SITE
// mask as taken from prjxray by symmetry rather than measured in a bitstream.
void ReportPrePass(const xilinx::PrePassReport& rep) {
	std::cerr << "xc7frames2bit: pre-pass " << rep.clb_frames
	          << " CLB_IO_CLK frame(s) masked, " << rep.verbatim_frames
	          << " frame(s) of other block types written verbatim; "
	          << rep.withheld_bits << " pass-2 bit(s) withheld"
	          << std::endl;
	if (rep.inferred_cin_x1_bits || rep.inferred_dsp_reg0_bits)
		std::cerr << "xc7frames2bit: of those, "
		          << rep.inferred_cin_x1_bits
		          << " were withheld by the INFERRED SLICE*_X1 "
		             "PRECYINIT.CIN bit (31_12) and "
		          << rep.inferred_dsp_reg0_bits
		          << " by the INFERRED DSP {A,B}REG_0 minor-27 bits. "
		             "Neither has been seen in a Vivado bitstream; see "
		             "docs/far-double-write-verdict.md sections 9.2 "
		             "and 9.3."
		          << std::endl;
	if (!rep.columns_without_geometry.empty()) {
		std::cerr << "xc7frames2bit: WARNING: "
		          << rep.columns_without_geometry.size()
		          << " configuration column(s) written have no tile at "
		             "all in tilegrid.json, so pass 1 withholds ALL of "
		             "the module's site content there. First:";
		size_t n = 0;
		for (uint32_t c : rep.columns_without_geometry) {
			if (n++ == 4)
				break;
			std::cerr << " 0x" << std::hex << std::setw(8)
			          << std::setfill('0') << c << std::dec;
		}
		std::cerr << std::endl;
	}
	if (!rep.unclassified_types_touched.empty()) {
		std::cerr << "xc7frames2bit: NOTE: the frames written cover "
		             "bit-bearing tile type(s) section 5.1's table does "
		             "not name and no bench has exercised --";
		for (const std::string& t : rep.unclassified_types_touched)
			std::cerr << " " << t;
		std::cerr << ". They keep nothing, per \"anything else -> "
		             "nothing\"."
		          << std::endl;
	}
	if (rep.verbatim_frames_with_nonzero_word50)
		std::cerr << "xc7frames2bit: NOTE: "
		          << rep.verbatim_frames_with_nonzero_word50
		          << " non-CLB_IO_CLK frame(s) of the input carry a "
		             "nonzero word 50. Vivado writes none: word 50 is "
		             "zero in 772 of 772 BLOCK_RAM frames. Copied "
		             "through unchanged, as the block-type rule "
		             "requires."
		          << std::endl;
}

// Read the file we just wrote, through the bitstream READER -- a different code
// path from the writer, which is the whole point -- and check that every frame
// of the input landed at its address with its exact payload.
//
// For a double-written stream the reader's frame map keeps the LAST write of
// each address, which is pass 2, which is the configuration. So this verifies
// the pass that matters and is blind to the pre-pass by construction; the
// pre-pass is checked structurally at build time instead, because a frame-level
// comparator is definitionally blind to it.
//
// `flush` names the frames the stream writes PAST the end of a FAR block that
// stops mid-row, and `flush_source` says what each must contain. They are
// outside --frm_file by construction, so "the caller did not ask for this
// address" -- the right verdict for every other extra -- is the wrong one here.
// They are not waived either: an extra address must be a declared flush
// address AND carry the static's exact words, so this is a second reading of
// the flush frame through the decoder rather than a hole in the verifier.
template <typename ArchType>
int VerifyRoundTrip(
    const std::string& path,
    const typename ArchType::Part& part,
    const typename xilinx::Frames<ArchType>::Frames2Data& want,
    const std::set<uint32_t>& flush = std::set<uint32_t>(),
    const typename xilinx::Frames<ArchType>::Frames2Data& flush_source =
        typename xilinx::Frames<ArchType>::Frames2Data()) {
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
	size_t flush_seen = 0, flush_wrong = 0;
	uint32_t first_flush_wrong = 0;
	for (const auto& frame : got) {
		if (want.find(frame.first) != want.end())
			continue;
		const uint32_t a = static_cast<uint32_t>(frame.first);
		if (!flush.count(a)) {
			if (!extra++)
				first_extra = a;
			continue;
		}
		++flush_seen;
		auto src = flush_source.find(frame.first);
		if (src == flush_source.end() ||
		    src->second.size() != frame.second.size() ||
		    !std::equal(src->second.begin(), src->second.end(),
		                frame.second.begin())) {
			if (!flush_wrong++)
				first_flush_wrong = a;
		}
	}
	if (flush_seen != flush.size()) {
		std::cerr << "xc7frames2bit: --verify FAILED. The stream "
		             "declares "
		          << flush.size() << " flush frame(s) and the decoder "
		                             "reads back "
		          << flush_seen
		          << ". A block that stops mid-row and writes no frame "
		             "past its last never commits that last frame."
		          << std::endl;
		return 1;
	}
	if (flush_wrong) {
		std::cerr << "xc7frames2bit: --verify FAILED. " << flush_wrong
		          << " flush frame(s) do not carry the static's own "
		             "content, first 0x"
		          << std::hex << std::setw(8) << std::setfill('0')
		          << first_flush_wrong << std::dec << "." << std::endl;
		return 1;
	}
	if (!missing && !wrong && !extra) {
		std::cerr << "xc7frames2bit: --verify round-trip OK, "
		          << want.size() << " frames, ECC included";
		if (!flush.empty())
			std::cerr << ", plus " << flush.size()
			          << " flush frame(s) carrying the static";
		std::cerr << std::endl;
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
			// The static's frames, read ONCE and used twice: as the
			// pre-pass's STATIC term, and as the source of every
			// block's flush frame. Declared here rather than inside
			// the pre-pass branch because a mid-row block needs the
			// flush frame under --prepass off too.
			xilinx::Frames<ArchType> static_frames;
			// Filled by checkPartialFrameSet; read by --verify,
			// which would otherwise convict the flush frames as
			// addresses the caller never asked for.
			std::set<uint32_t> flush_addresses;
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

				// READ BEFORE THE FIRST EMISSION, because the
				// flush frame's content comes from here and
				// the writer refuses rather than zero-filling.
				if (have_static &&
				    static_frames.readFrames(
				        FLAGS_static_frames)) {
					throw std::runtime_error(
					    "Static frames file " +
					    FLAGS_static_frames +
					    " not found or invalid");
				}

				cfg_packets = xilinx::Configuration<ArchType>::
				    createPartialPacketData(
				        frames.getFrames(),
				        static_frames.getFrames(), part,
				        FLAGS_pad_frames);

				// Unconditional, per Unit 6.3 step 5: the
				// check runs on every emission rather than
				// behind a flag someone has to remember.
				size_t emitted =
				    xilinx::Configuration<ArchType>::
				        checkPartialFrameSet(
				            cfg_packets, frames.getFrames(),
				            static_frames.getFrames(), part,
				            FLAGS_pad_frames, &flush_addresses);

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
					// SITE is per tile, so the pre-pass is a
					// function of prjxray's tile lattice.
					// No geometry, no pre-pass -- there is
					// no degraded mask to fall back to.
					std::string geo_error;
					auto geometry =
					    LoadPrePassGeometry(&geo_error);
					if (!geometry)
						throw std::runtime_error(
						    "Cannot build the "
						    "pre-pass SITE mask: " +
						    geo_error);
					std::cerr << "xc7frames2bit: "
					          << geometry->summary()
					          << std::endl;
					xilinx::PrePassReport prepass_report;
					prepass_frames =
					    xilinx::Configuration<ArchType>::
					        createPrePassFrames(
					            frames.getFrames(),
					            static_frames.getFrames(),
					            use_static, *geometry,
					            &prepass_report);
					ReportPrePass(prepass_report);
					auto prepass_blocks =
					    xilinx::Configuration<ArchType>::
					        createPartialPacketData(
					            prepass_frames,
					            static_frames.getFrames(),
					            part, FLAGS_pad_frames);
					// Both passes must land on exactly the
					// same frames. Checking pass 1's frame
					// set separately is what stops a
					// pre-pass from writing outside the
					// region.
					size_t emitted1 =
					    xilinx::Configuration<ArchType>::
					        checkPartialFrameSet(
					            prepass_blocks,
					            prepass_frames,
					            static_frames.getFrames(),
					            part, FLAGS_pad_frames);
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

				// The trailing-frame accounting is PRINTED, not
				// left to be inferred from the byte count. A
				// stream whose blocks stop on their last real
				// frame is the defect measured on silicon
				// 2026-09-03 (71 of 72 committed), and it is
				// invisible in every other number on this line.
				size_t stream_frames = 0;
				for (const auto& b : cfg_packets)
					stream_frames +=
					    b.second.size() /
					    ArchType::words_per_frame;
				std::cerr << "xc7frames2bit: partial "
				             "bitstream, "
				          << emitted << " frames in "
				          << cfg_packets.size()
				          << " FAR block(s), "
				          << (want_prepass ? 2 : 1)
				          << " pass(es), "
				          << all_blocks.size()
				          << " FDRI block(s) emitted; "
				          << (stream_frames - emitted)
				          << " trailing frame(s) per pass, of "
				             "which "
				          << flush_addresses.size()
				          << " flush frame(s) carrying the "
				             "static"
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
			    VerifyRoundTrip<ArchType>(
			        FLAGS_output_file, *part, frames.getFrames(),
			        flush_addresses, static_frames.getFrames()))
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
			xilinx::FullStreamOptions full_options;
			full_options.emit_crc = FLAGS_emit_crc;
			xilinx::CompressedFrameData compressed;
			if (FLAGS_compress) {
				try {
					compressed = xilinx::Configuration<
					    ArchType>::
					    createCompressedFrameData(
					        frames.getFrames(), part);
					size_t committed =
					    xilinx::Configuration<ArchType>::
					        checkCompressedFrameSet(
					            compressed,
					            frames.getFrames(), part);
					size_t burst_frames = 0, replays = 0,
					       episodes = 0;
					for (const auto& b : compressed) {
						burst_frames +=
						    b.words.size() /
						    ArchType::words_per_frame;
						replays += b.replay.size();
						if (b.mfw)
							++episodes;
					}
					std::cerr
					    << "xc7frames2bit: multiple-frame "
					       "write, "
					    << committed << " frame(s) in "
					    << compressed.size()
					    << " FDRI burst(s) carrying "
					    << burst_frames << " frame(s), "
					    << episodes << " MFW episode(s), "
					    << replays << " replay(s)"
					    << std::endl;
				} catch (const std::runtime_error& err) {
					std::cerr << err.what() << std::endl;
					return 1;
				}
				full_options.compressed = &compressed;
			}
			if (FLAGS_rbcrc != "off") {
				xc7series::ConfigurationOptions1Value::
				    ReadbackCrcAction action;
				if (!ParseRbcrcAction(FLAGS_rbcrc, &action)) {
					std::cerr
					    << "--rbcrc must be off, continue, "
					       "halt, correct_and_halt or "
					       "correct_and_continue."
					    << std::endl;
					return 1;
				}
				full_options.cor1 =
				    xc7series::ConfigurationOptions1Value()
				        .SetContinuousReadbackCrc(true)
				        .SetReadbackCrcAction(action)
				        .SetReadbackCrcErrorPinDisabled(
				            !FLAGS_rbcrc_error_pin);
			}
			try {
				xilinx::Configuration<ArchType>::
				    createConfigurationPackage(
				        configuration_package,
				        configuration_packet_data, part,
				        full_options);
			} catch (const std::runtime_error& e) {
				std::cerr << e.what() << std::endl;
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
	// A flag that silently does nothing is a check that never runs. --crc
	// steers the partial path and --emit_crc steers the full one; asking
	// for the wrong one is a refusal, not a no-op.
	if (FLAGS_emit_crc && FLAGS_partial_bitstream) {
		std::cerr << "--emit_crc is the FULL bitstream switch. A "
		             "partial bitstream's CRC is controlled by --crc, "
		             "which already defaults to compute."
		          << std::endl;
		return 1;
	}
	// Same rule, second edge. createConfigurationPackage() takes emit_crc on
	// all four architecture specializations, but only the Series7 body acts
	// on it: the CRC model (computeStreamCrc) is measured for 7-series and
	// throws for everything else, and the two substitution positions were
	// read out of a 7-series Vivado bitstream. Accepting --emit_crc for
	// another architecture would emit a CRC-DISABLED stream at exit 0 with
	// the caller believing otherwise -- which is the exact shape of the
	// defect this flag exists to close.
	if (FLAGS_emit_crc && FLAGS_architecture != "Series7") {
		std::cerr << "--emit_crc is characterised for Series7 only and "
		             "--architecture is "
		          << FLAGS_architecture
		          << ". The stream CRC model and the two write "
		             "positions are both 7-series measurements; there "
		             "is no degraded version to fall back to."
		          << std::endl;
		return 1;
	}
	// --rbcrc has the same two edges as --emit_crc and for the same reason:
	// COR1 is written by the FULL path only -- the Series7 partial writer
	// touches CMD, IDCODE, MASK, CTL0, FAR, FDRI and CRC and no other
	// register -- and UG470 Table 5-33's layout is a 7-series layout.
	//
	// That the partial path leaves COR1 alone is the useful half: RBCRC_EN
	// established by the static's full bitstream survives every partial
	// load that follows, so this flag belongs on the static and nowhere
	// else. Accepting it on a partial would silently produce a stream that
	// enables nothing.
	if (FLAGS_rbcrc != "off" && FLAGS_partial_bitstream) {
		std::cerr << "--rbcrc is the FULL bitstream switch. COR1 is "
		             "written only by the full path, and a partial "
		             "bitstream does not clear it: set --rbcrc when "
		             "building the STATIC, and every partial loaded "
		             "over it keeps the setting."
		          << std::endl;
		return 1;
	}
	if (FLAGS_rbcrc != "off" && FLAGS_architecture != "Series7") {
		std::cerr << "--rbcrc is characterised for Series7 only and "
		             "--architecture is "
		          << FLAGS_architecture
		          << ". COR1's field layout is UG470 Table 5-33's; "
		             "Spartan6 numbers the register differently and "
		             "UltraScale/UltraScale+ follow UG570, which is "
		             "uncharacterised here."
		          << std::endl;
		return 1;
	}
	// A flag that silently does nothing is a check that never runs.
	// --rbcrc_error_pin only reaches the stream through the COR1 override
	// --rbcrc builds, so passing it alone asks for a bit that is never
	// written.
	if (!FLAGS_rbcrc_error_pin && FLAGS_rbcrc == "off") {
		std::cerr << "--norbcrc_error_pin sets COR1[9] RBCRC_NO_PIN, "
		             "which is only written when --rbcrc is on. With "
		             "--rbcrc off no COR1 override is emitted at all "
		             "and the flag would do nothing."
		          << std::endl;
		return 1;
	}
	if (FLAGS_compress && FLAGS_partial_bitstream) {
		std::cerr << "--compress is the FULL bitstream switch. The "
		             "partial path emits one FAR block per run of "
		             "consecutive frames and has no MFW episode; a "
		             "partial's frames are also the dense ones, which "
		             "is the population this encoding cannot shrink."
		          << std::endl;
		return 1;
	}
	if (FLAGS_compress && FLAGS_architecture != "Series7") {
		std::cerr << "--compress is characterised for Series7 only and "
		             "--architecture is "
		          << FLAGS_architecture
		          << ". The MFWR packet grammar -- eight zero words for "
		             "the write that carries no FAR, four for each "
		             "replay -- was read out of Vivado's compressed "
		             "7-series bitstreams; there is no degraded "
		             "version to fall back to."
		          << std::endl;
		return 1;
	}
	xilinx::Architecture::Container arch_container =
	    xilinx::ArchitectureFactory::create_architecture(
	        FLAGS_architecture);
	return absl::visit(Frames2BitWriter(), arch_container);
}
