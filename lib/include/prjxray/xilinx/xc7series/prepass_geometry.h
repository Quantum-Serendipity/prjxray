/*
 * Copyright (C) 2017-2020  The Project X-Ray Authors.
 *
 * Use of this source code is governed by a ISC-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/ISC
 *
 * SPDX-License-Identifier: ISC
 */
#ifndef PRJXRAY_LIB_XILINX_XC7SERIES_PREPASS_GEOMETRY_H_
#define PRJXRAY_LIB_XILINX_XC7SERIES_PREPASS_GEOMETRY_H_

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <prjxray/xilinx/xc7series/frame_address.h>
#include <prjxray/xilinx/xc7series/part.h>

namespace prjxray {
namespace xilinx {
namespace xc7series {

// The geometry the FAR double write's first pass is a function of.
//
// docs/far-double-write-verdict.md section 5.1: the SITE mask is PER TILE, not
// per (minor, word). A frame word is shared between the tiles whose bit ranges
// cover it, and which of them pass 1 keeps depends on what KIND of tile it is:
//
//   for a frame at configuration column c, minor m, word w, take the bitwise OR
//   over every tile of column c whose offset <= w < offset + words, of
//
//     w == 50                  ->  nothing        word 50 is the HCLK tile's
//     CLB*  m == 0             ->  tile-bit index == 0 (mod 4)
//           m == 1             ->  tile-bit index == 3 (mod 4)
//           26 <= m <= 35      ->  everything EXCEPT *.PRECYINIT.CIN
//     BRAM* m == 27            ->  everything
//           anything else      ->  nothing
//     DSP*  26 <= m <= 27      ->  everything EXCEPT the tile's own input-mux
//                                  bits: *.DSP_GND_*/*.DSP_VCC_*, plus
//                                  {A,B}REG_0's minor-27 bit
//     INT*, HCLK*, anything else -> nothing
//
// The tile spans come from prjxray's tilegrid.json; the excluded bit positions
// come from prjxray's own segbits, not from a table copied out of the document,
// so a database revision that moves them is a load failure rather than a silent
// wrong answer. See the two literal cross-checks in the .cc.
//
// The predecessor of this class was a free function SiteMask(minor, word)
// returning 0x11111111 / 0x88888888 / 0xFFFFFFFF. That form agrees with this
// one in a CLB configuration column apart from PRECYINIT.CIN, and is wrong by
// thousands of bits in a DSP or BRAM column -- 2,987 in one module of the
// round-2 corpus (verdict section 9.3). It survived Unit 6.2 because no module
// in that corpus placed a DSP or a BRAM.
class PrePassGeometry {
       public:
	static constexpr size_t kWordsPerFrame = 101;
	static constexpr size_t kEccWord = 0x32;

	// What the tile kinds are called in section 5.1's table.
	enum class Kind { kClb, kBram, kDsp, kInterconnect, kUnclassified };

	// The masks for one frame. All three vectors are kWordsPerFrame long.
	struct FrameMasks {
		// The bit positions pass 1 keeps of the RM.
		std::vector<uint32_t> site;

		// The two elements of the mask that section 5.1 flags as NOT
		// fully measured, kept separate so a mismatch is diagnosable
		// rather than mysterious. Both are subsets of ~site.
		//
		//   inferred_cin_x1 -- bit 31_12, the SLICE*_X1 half of the
		//   PRECYINIT.CIN exclusion. All 66 measured instances are
		//   SLICE*_X0 (30_13); Vivado never placed a CARRY4 in the _X1
		//   slice in either bench, so _X1 is taken from prjxray's
		//   database by symmetry. Verdict section 9.2.
		//
		//   inferred_dsp_reg0 -- 27_38, 27_111, 27_198 and 27_271, the
		//   minor-27 bit of each {A,B}REG_0 code. prjxray's DSP
		//   database is not fine-grained enough to name them as a mux;
		//   they are the one fitted element of the corrected mask.
		//   Verdict section 9.3.
		std::vector<uint32_t> inferred_cin_x1;
		std::vector<uint32_t> inferred_dsp_reg0;

		// False when tilegrid.json has no tile at all in this
		// configuration column, so `site` is all zeros for want of
		// geometry rather than because section 5.1 says so. 12 of
		// xc7a100t's 234 block-0 columns are in this state.
		bool column_in_tilegrid = false;

		// Bit-bearing tile types in this column that section 5.1's
		// table does not name -- IO, CMT, CLK, PCIE, GTP, CFG. They
		// contribute nothing under "anything else -> nothing", which is
		// what section 5.1 states and what no bench has yet exercised.
		// Points into the geometry; never null.
		const std::set<std::string>* unclassified_types = nullptr;
	};

	// db_root is the FAMILY directory -- the one holding segbits_*.db and
	// mapping/parts.yaml, e.g. <prjxray-db>/artix7. The fabric directory
	// that carries tilegrid.json is resolved through prjxray's own
	// mapping/parts.yaml -> mapping/devices.yaml chain, the same two files
	// prjxray/util.py:get_fabric_for_part walks.
	static std::unique_ptr<PrePassGeometry> LoadForPart(
	    const std::string& db_root,
	    const std::string& part,
	    std::string* error);

	// Explicit paths. segbits_dir holds segbits_<tiletype>.db.
	static std::unique_ptr<PrePassGeometry> Load(
	    const std::string& tilegrid_path,
	    const std::string& segbits_dir,
	    std::string* error);

	// Cached; the caller may hold the reference until the next call.
	static bool FabricForPart(const std::string& db_root,
	                          const std::string& part,
	                          std::string* fabric,
	                          std::string* error);

	std::vector<uint32_t> ColumnsNotInPart(const Part& part) const;

	const FrameMasks& MasksFor(const FrameAddress& address) const;

	// One line for stderr: what was loaded and from where. A geometry that
	// is not reported is a geometry nobody checked.
	const std::string& summary() const { return summary_; }

	// Every bit-bearing tile type the tilegrid carries that section 5.1's
	// table does not name, with its tile count. Device-wide, not restricted
	// to the frames being written.
	const std::map<std::string, size_t>& unclassified_device_wide() const {
		return unclassified_device_wide_;
	}

       private:
	struct TileType {
		std::string name;
		Kind kind = Kind::kUnclassified;
		// minor -> word-within-tile -> bits to clear from `site`
		std::map<uint8_t, std::map<uint16_t, uint32_t>> exclude;
		std::map<uint8_t, std::map<uint16_t, uint32_t>>
		    exclude_inferred_cin;
		std::map<uint8_t, std::map<uint16_t, uint32_t>>
		    exclude_inferred_dsp;
	};

	struct TileSpan {
		const TileType* type;
		uint16_t offset;
		uint16_t words;
	};

	struct Column {
		std::vector<TileSpan> spans;
		std::set<std::string> unclassified;
	};

	PrePassGeometry() = default;

	std::map<std::string, TileType> types_;
	std::map<uint32_t, Column> columns_;  // key: FAR with minor == 0
	std::map<std::string, size_t> unclassified_device_wide_;
	std::string summary_;
	std::set<std::string> empty_types_;

	// keyed by (column key | minor)
	mutable std::map<uint32_t, FrameMasks> cache_;
};

}  // namespace xc7series
}  // namespace xilinx
}  // namespace prjxray

#endif  // PRJXRAY_LIB_XILINX_XC7SERIES_PREPASS_GEOMETRY_H_
