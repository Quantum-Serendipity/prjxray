/*
 * Copyright (C) 2017-2020  The Project X-Ray Authors.
 *
 * Use of this source code is governed by a ISC-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/ISC
 *
 * SPDX-License-Identifier: ISC
 */
#include <prjxray/xilinx/xc7series/prepass_geometry.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace xc7series = prjxray::xilinx::xc7series;
using xc7series::BlockType;
using xc7series::FrameAddress;
using xc7series::PrePassGeometry;

namespace {

constexpr char kTileGrid[] = "prepass_tilegrid.json";
constexpr char kDb[] = "prepass_db";

// The fixture's five columns. Column 5 is deliberately not in the tilegrid.
FrameAddress Clb(uint8_t minor) {
	return FrameAddress(BlockType::CLB_IO_CLK, false, 0, 0, minor);
}
FrameAddress Dsp(uint8_t minor) {
	return FrameAddress(BlockType::CLB_IO_CLK, false, 0, 1, minor);
}
FrameAddress Bram(uint8_t minor) {
	return FrameAddress(BlockType::CLB_IO_CLK, false, 0, 2, minor);
}
FrameAddress Io(uint8_t minor) {
	return FrameAddress(BlockType::CLB_IO_CLK, false, 0, 3, minor);
}
FrameAddress ClbmR(uint8_t minor) {
	return FrameAddress(BlockType::CLB_IO_CLK, false, 0, 4, minor);
}
FrameAddress Nowhere(uint8_t minor) {
	return FrameAddress(BlockType::CLB_IO_CLK, false, 0, 5, minor);
}

std::unique_ptr<PrePassGeometry> Good() {
	std::string err;
	auto geo = PrePassGeometry::Load(kTileGrid, kDb, &err);
	EXPECT_TRUE(geo) << err;
	return geo;
}

// Copy the three fixture segbits files into a fresh directory, replacing one
// line, so every refusal below has a case that must load and a case that must
// not, differing by exactly the thing the check is about.
class DoctoredDb {
       public:
	DoctoredDb(const std::string& from, const std::string& to) {
		char tmpl[] = "/tmp/prjxray_prepass_db_XXXXXX";
		const char* d = mkdtemp(tmpl);
		EXPECT_NE(d, nullptr);
		dir_ = d ? d : "";
		for (const char* f : {"segbits_clbll_l.db",
		                      "segbits_clblm_r.db",
		                      "segbits_dsp_l.db"}) {
			std::ifstream in(std::string(kDb) + "/" + f);
			std::ofstream out(dir_ + "/" + f);
			std::string line;
			while (std::getline(in, line)) {
				size_t at = line.find(from);
				if (!from.empty() && at != std::string::npos) {
					if (to.empty())
						continue;  // drop the line
					line.replace(at, from.size(), to);
				}
				out << line << "\n";
			}
		}
	}
	~DoctoredDb() {
		for (const char* f : {"segbits_clbll_l.db",
		                      "segbits_clblm_r.db",
		                      "segbits_dsp_l.db"})
			std::remove((dir_ + "/" + f).c_str());
		std::remove(dir_.c_str());
	}
	const std::string& dir() const { return dir_; }

       private:
	std::string dir_;
};

}  // namespace

TEST(PrePassGeometryTest, LoadsTheFixtureAndSaysWhatItLoaded) {
	auto geo = Good();
	ASSERT_TRUE(geo);
	EXPECT_NE(geo->summary().find("5 configuration column(s)"),
	          std::string::npos)
	    << geo->summary();
	// The IO column's tile type is not one section 5.1's table names, and
	// the geometry says so rather than quietly treating it as a site tile.
	ASSERT_EQ(geo->unclassified_device_wide().size(), 1u);
	EXPECT_EQ(geo->unclassified_device_wide().begin()->first, "LIOI3");
}

// Section 5.1: CLB, minor 0 -> tile-bit index == 0 (mod 4); minor 1 -> == 3.
TEST(PrePassGeometryTest, ClbColumnKeepsTheModFourLattice) {
	auto geo = Good();
	ASSERT_TRUE(geo);
	const auto& m0 = geo->MasksFor(Clb(0));
	const auto& m1 = geo->MasksFor(Clb(1));
	EXPECT_TRUE(m0.column_in_tilegrid);
	for (size_t w : {0u, 1u, 99u, 100u}) {
		EXPECT_EQ(m0.site[w], 0x11111111u) << "word " << w;
		EXPECT_EQ(m1.site[w], 0x88888888u) << "word " << w;
	}
	// Words no tile of the fixture covers own nothing -- so the assertions
	// above are about the CLB tile and not about a default.
	for (size_t w : {2u, 49u, 51u, 98u}) {
		EXPECT_EQ(m0.site[w], 0u) << "word " << w;
		EXPECT_EQ(m1.site[w], 0u) << "word " << w;
	}
}

// Word 50 is the HCLK tile's alone, at every minor and in every column class,
// even where a DSP or BRAM tile's ten-word span runs straight through it.
TEST(PrePassGeometryTest, Word50IsNeverKept) {
	auto geo = Good();
	ASSERT_TRUE(geo);
	for (int m = 0; m < 36; ++m) {
		EXPECT_EQ(geo->MasksFor(Clb(m)).site[50], 0u) << "CLB " << m;
		EXPECT_EQ(geo->MasksFor(Dsp(m)).site[50], 0u) << "DSP " << m;
		EXPECT_EQ(geo->MasksFor(Bram(m)).site[50], 0u) << "BRAM " << m;
	}
}

// The CLB site planes keep everything EXCEPT *.PRECYINIT.CIN -- 30_13 for
// SLICE*_X0, 31_12 for SLICE*_X1. Withheld 66 of 66 in the round-2 corpus.
TEST(PrePassGeometryTest, ClbSitePlanesExcludePrecyinitCin) {
	auto geo = Good();
	ASSERT_TRUE(geo);
	const auto& m30 = geo->MasksFor(Clb(30));
	const auto& m31 = geo->MasksFor(Clb(31));
	EXPECT_EQ(m30.site[0], 0xFFFFFFFFu & ~(1u << 13));
	EXPECT_EQ(m31.site[0], 0xFFFFFFFFu & ~(1u << 12));
	// The exclusion is at tile-bit 13, i.e. the tile's FIRST word; its
	// second word keeps everything.
	EXPECT_EQ(m30.site[1], 0xFFFFFFFFu);
	EXPECT_EQ(m31.site[1], 0xFFFFFFFFu);
	// ...and at the tile 99 words further up, because the exclusion is
	// per tile and not per frame word.
	EXPECT_EQ(m30.site[99], 0xFFFFFFFFu & ~(1u << 13));
	EXPECT_EQ(m31.site[99], 0xFFFFFFFFu & ~(1u << 12));
	EXPECT_EQ(m30.site[100], 0xFFFFFFFFu);
	// PRECYINIT.AX (30_14) is KEPT, 18 of 18 -- so what pass 1 withholds is
	// the dedicated inter-tile cascade, not every signal from outside.
	EXPECT_TRUE(m30.site[0] & (1u << 14));
	// The SLICE*_X1 half is the inferred one, and it is flagged as such.
	EXPECT_EQ(m31.inferred_cin_x1[0], 1u << 12);
	EXPECT_EQ(m30.inferred_cin_x1[0], 0u);
	EXPECT_EQ(m31.inferred_dsp_reg0[0], 0u);
}

// A second CLB tile type, so the per-type segbits read is exercised rather than
// one file being loaded and reused.
TEST(PrePassGeometryTest, EachClbTypeReadsItsOwnSegbits) {
	auto geo = Good();
	ASSERT_TRUE(geo);
	EXPECT_EQ(geo->MasksFor(ClbmR(30)).site[0], 0xFFFFFFFFu & ~(1u << 13));
	EXPECT_EQ(geo->MasksFor(ClbmR(31)).site[0], 0xFFFFFFFFu & ~(1u << 12));
	EXPECT_EQ(geo->MasksFor(ClbmR(0)).site[0], 0x11111111u);
}

// THE 2,987-BIT BUG. A BRAM configuration column keeps minor 27 and nothing
// else. The mask this replaces returned 0xFFFFFFFF for every minor in 26..35
// and so kept minor 26 -- the BRAM's address-input mux plane, 2,026 bits in one
// module -- which Vivado withholds.
TEST(PrePassGeometryTest, BramColumnKeepsMinor27AndNothingElse) {
	auto geo = Good();
	ASSERT_TRUE(geo);
	const auto& m27 = geo->MasksFor(Bram(27));
	for (size_t w = 0; w < 10; ++w)
		EXPECT_EQ(m27.site[w], 0xFFFFFFFFu) << "word " << w;
	for (int m : {0, 1, 2, 24, 25, 26}) {
		const auto& mm = geo->MasksFor(Bram(m));
		for (size_t w = 0; w < PrePassGeometry::kWordsPerFrame; ++w)
			EXPECT_EQ(mm.site[w], 0u)
			    << "BRAM minor " << m << " word " << w;
	}
}

// A DSP column keeps minors 26-27 except the tile's own input tie-off muxes and
// the {A,B}REG_0 minor-27 bit. The same pre-round-2 mask kept all of them.
TEST(PrePassGeometryTest, DspColumnExcludesItsOwnInputMuxes) {
	auto geo = Good();
	ASSERT_TRUE(geo);
	const auto& m26 = geo->MasksFor(Dsp(26));
	const auto& m27 = geo->MasksFor(Dsp(27));
	// 26_63 -> word 1 bit 31, 26_50 -> word 1 bit 18, 26_71 -> word 2 bit 7
	EXPECT_EQ(m26.site[1], 0xFFFFFFFFu & ~(1u << 31) & ~(1u << 18));
	EXPECT_EQ(m26.site[2], 0xFFFFFFFFu & ~(1u << 7));
	// AREG_0's minor-26 bits (26_113, 26_137) are KEPT: only the minor-27
	// bit of the code is withheld.
	EXPECT_EQ(m26.site[3], 0xFFFFFFFFu);
	EXPECT_EQ(m26.site[4], 0xFFFFFFFFu);
	// 27_38 -> word 1 bit 6 (inferred), 27_51 -> word 1 bit 19,
	// 27_62 -> word 1 bit 30.
	EXPECT_EQ(m27.site[1],
	          0xFFFFFFFFu & ~(1u << 6) & ~(1u << 19) & ~(1u << 30));
	// 27_111 -> word 3 bit 15, inferred and nothing else in that word.
	EXPECT_EQ(m27.site[3], 0xFFFFFFFFu & ~(1u << 15));
	EXPECT_EQ(m27.inferred_dsp_reg0[3], 1u << 15);
	EXPECT_EQ(m27.inferred_dsp_reg0[1], 1u << 6);
	EXPECT_EQ(m26.inferred_dsp_reg0[3], 0u);
	// Minors outside 26-27 own nothing in a DSP column, including 0 and 1
	// where the CLB lattice would otherwise apply.
	for (int m : {0, 1, 2, 25}) {
		const auto& mm = geo->MasksFor(Dsp(m));
		for (size_t w = 0; w < PrePassGeometry::kWordsPerFrame; ++w)
			EXPECT_EQ(mm.site[w], 0u)
			    << "DSP minor " << m << " word " << w;
	}
}

// "INT*, HCLK*, anything else -> nothing" -- stated by section 5.1 and
// exercised by no bench, so the type is named in the report rather than
// silently folded into the interconnect.
TEST(PrePassGeometryTest, UnclassifiedColumnOwnsNothingAndSaysSo) {
	auto geo = Good();
	ASSERT_TRUE(geo);
	const auto& m = geo->MasksFor(Io(30));
	EXPECT_TRUE(m.column_in_tilegrid);
	for (size_t w = 0; w < PrePassGeometry::kWordsPerFrame; ++w)
		EXPECT_EQ(m.site[w], 0u) << "word " << w;
	ASSERT_NE(m.unclassified_types, nullptr);
	EXPECT_EQ(m.unclassified_types->count("LIOI3"), 1u);
	// A CLB column has nothing to report.
	EXPECT_TRUE(geo->MasksFor(Clb(30)).unclassified_types->empty());
}

// 12 of xc7a100t's 234 block-0 columns have no tile in tilegrid.json at all.
// The mask is empty there for want of geometry, which is not the same statement
// as "section 5.1 says nothing is kept", and the caller is told which it is.
TEST(PrePassGeometryTest, ColumnAbsentFromTheTilegridIsFlagged) {
	auto geo = Good();
	ASSERT_TRUE(geo);
	const auto& m = geo->MasksFor(Nowhere(30));
	EXPECT_FALSE(m.column_in_tilegrid);
	for (size_t w = 0; w < PrePassGeometry::kWordsPerFrame; ++w)
		EXPECT_EQ(m.site[w], 0u);
	EXPECT_TRUE(geo->MasksFor(Clb(30)).column_in_tilegrid);
}

// ---------------------------------------------------------------- refusals --
// The excluded positions are read out of prjxray's database, and section 5.1
// names the values it measured. If the two disagree, the measurement is what
// stops applying.

TEST(PrePassGeometryTest, RefusesADatabaseWhosePrecyinitCinHasMoved) {
	DoctoredDb db("30_13", "30_15");
	std::string err;
	auto geo = PrePassGeometry::Load(kTileGrid, db.dir(), &err);
	EXPECT_FALSE(geo);
	EXPECT_NE(err.find("PRECYINIT.CIN"), std::string::npos) << err;
	// ...and the same load from the undoctored directory succeeds, so the
	// refusal is about the moved bit and not about the temporary copy.
	std::string ok_err;
	EXPECT_TRUE(PrePassGeometry::Load(kTileGrid, kDb, &ok_err)) << ok_err;
}

TEST(PrePassGeometryTest, RefusesADatabaseWhoseDspRegBitsHaveMoved) {
	DoctoredDb db("27_111", "27_112");
	std::string err;
	auto geo = PrePassGeometry::Load(kTileGrid, db.dir(), &err);
	EXPECT_FALSE(geo);
	EXPECT_NE(err.find("{A,B}REG_0"), std::string::npos) << err;
}

TEST(PrePassGeometryTest, RefusesADatabaseWithoutTheDspInputMuxes) {
	DoctoredDb db("DSP_GND_L", "");  // drop every GND line
	std::string err;
	auto geo = PrePassGeometry::Load(kTileGrid, db.dir(), &err);
	EXPECT_FALSE(geo);
	EXPECT_NE(err.find("input muxes are missing"), std::string::npos)
	    << err;
}

TEST(PrePassGeometryTest, RefusesWhenASegbitsFileIsMissing) {
	std::string err;
	auto geo = PrePassGeometry::Load(kTileGrid, "no_such_db_dir", &err);
	EXPECT_FALSE(geo);
	EXPECT_NE(err.find("segbits_"), std::string::npos) << err;
}

TEST(PrePassGeometryTest, RefusesAMissingTilegrid) {
	std::string err;
	auto geo = PrePassGeometry::Load("no_such_tilegrid.json", kDb, &err);
	EXPECT_FALSE(geo);
	EXPECT_NE(err.find("no_such_tilegrid.json"), std::string::npos) << err;
}
