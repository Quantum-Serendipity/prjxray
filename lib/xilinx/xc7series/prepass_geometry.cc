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

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

#include <prjxray/segbits_file_reader.h>
#include <yaml-cpp/yaml.h>

namespace prjxray {
namespace xilinx {
namespace xc7series {

namespace {

// FAR field layout, UG470 table 5-24. Only block type 0 is ever masked, so the
// key below is the whole address with the minor cleared -- which is exactly the
// `baseaddr` tilegrid.json records.
constexpr uint32_t kMinorMask = 0x7F;

std::string Lower(const std::string& s) {
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(),
	               [](unsigned char c) { return std::tolower(c); });
	return out;
}

bool StartsWith(const std::string& s, const char* prefix) {
	return s.compare(0, std::strlen(prefix), prefix) == 0;
}

// A prjxray segbit position, "<minor>_<tile-bit index>". The tile-bit index
// runs over the tile's OWN words: index = (word - offset) * 32 + bit.
struct Position {
	uint8_t minor;
	uint16_t word_in_tile;
	uint8_t bit;
};

bool ParsePosition(const std::string& token, Position* out) {
	if (token.empty())
		return false;
	size_t start = (token[0] == '!') ? 1 : 0;
	size_t sep = token.find('_', start);
	if (sep == std::string::npos)
		return false;
	unsigned long minor = 0, index = 0;
	try {
		minor = std::stoul(token.substr(start, sep - start));
		index = std::stoul(token.substr(sep + 1));
	} catch (const std::exception&) {
		return false;
	}
	if (minor > kMinorMask)
		return false;
	out->minor = static_cast<uint8_t>(minor);
	out->word_in_tile = static_cast<uint16_t>(index / 32);
	out->bit = static_cast<uint8_t>(index % 32);
	return true;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
	return s.size() >= suffix.size() &&
	       s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string PositionsToString(
    const std::map<uint8_t, std::map<uint16_t, uint32_t>>& m) {
	std::ostringstream o;
	bool first = true;
	for (const auto& minor : m) {
		for (const auto& word : minor.second) {
			for (int b = 0; b < 32; ++b) {
				if (!(word.second & (1u << b)))
					continue;
				if (!first)
					o << " ";
				first = false;
				o << static_cast<int>(minor.first) << "_"
				  << (word.first * 32 + b);
			}
		}
	}
	return o.str();
}

void AddPosition(std::map<uint8_t, std::map<uint16_t, uint32_t>>* m,
                 const Position& p) {
	(*m)[p.minor][p.word_in_tile] |= (1u << p.bit);
}

}  // namespace

const PrePassGeometry::FrameMasks& PrePassGeometry::MasksFor(
    const FrameAddress& address) const {
	const uint32_t key = static_cast<uint32_t>(address);
	auto cached = cache_.find(key);
	if (cached != cache_.end())
		return cached->second;

	const uint8_t minor = static_cast<uint8_t>(key & kMinorMask);
	const uint32_t column_key = key & ~kMinorMask;

	FrameMasks masks;
	masks.site.assign(kWordsPerFrame, 0);
	masks.inferred_cin_x1.assign(kWordsPerFrame, 0);
	masks.inferred_dsp_reg0.assign(kWordsPerFrame, 0);
	masks.unclassified_types = &empty_types_;

	auto col = columns_.find(column_key);
	if (col != columns_.end()) {
		masks.column_in_tilegrid = true;
		masks.unclassified_types = &col->second.unclassified;
		for (const TileSpan& span : col->second.spans) {
			const TileType& t = *span.type;
			// Which bits this KIND of tile owns at this minor.
			// Section 5.1's table, verbatim.
			bool lattice = false;
			int residue = 0;
			bool everything = false;
			switch (t.kind) {
				case Kind::kClb:
					if (minor == 0) {
						lattice = true;
						residue = 0;
					} else if (minor == 1) {
						lattice = true;
						residue = 3;
					} else if (minor >= 26 && minor <= 35) {
						everything = true;
					}
					break;
				case Kind::kBram:
					everything = (minor == 27);
					break;
				case Kind::kDsp:
					everything = (minor >= 26 &&
					              minor <= 27);
					break;
				case Kind::kInterconnect:
				case Kind::kUnclassified:
					break;
			}
			if (!lattice && !everything)
				continue;

			const auto ex = t.exclude.find(minor);
			const auto in_cin = t.exclude_inferred_cin.find(minor);
			const auto in_dsp = t.exclude_inferred_dsp.find(minor);
			for (uint16_t i = 0; i < span.words; ++i) {
				const size_t w =
				    static_cast<size_t>(span.offset) + i;
				// Word 50 is the HCLK tile's alone, at every
				// minor, whichever tile's span covers it.
				if (w == kEccWord || w >= kWordsPerFrame)
					continue;
				uint32_t base = 0xFFFFFFFF;
				if (lattice) {
					// "tile-bit index == residue (mod 4)".
					// The index is i * 32 + bit and 32 is
					// 0 mod 4, so this is the same
					// 0x11111111 / 0x88888888 in every word
					// of the tile -- but it is written as
					// the rule, not as the constant, so it
					// stays right for a tile whose span
					// does not start on a word boundary.
					base = 0;
					for (int b = 0; b < 32; ++b) {
						if (((static_cast<int>(i) * 32 +
						      b) &
						     3) == residue)
							base |= (1u << b);
					}
				}
				uint32_t clear = 0, cin = 0, dsp = 0;
				if (ex != t.exclude.end()) {
					auto it = ex->second.find(i);
					if (it != ex->second.end())
						clear = it->second;
				}
				if (in_cin != t.exclude_inferred_cin.end()) {
					auto it = in_cin->second.find(i);
					if (it != in_cin->second.end())
						cin = it->second;
				}
				if (in_dsp != t.exclude_inferred_dsp.end()) {
					auto it = in_dsp->second.find(i);
					if (it != in_dsp->second.end())
						dsp = it->second;
				}
				masks.site[w] |= base & ~clear;
				masks.inferred_cin_x1[w] |= base & cin;
				masks.inferred_dsp_reg0[w] |= base & dsp;
			}
		}
	}
	// A bit another tile of the same word keeps is not withheld at all, so
	// it cannot be attributed to an inferred element.
	for (size_t w = 0; w < kWordsPerFrame; ++w) {
		masks.inferred_cin_x1[w] &= ~masks.site[w];
		masks.inferred_dsp_reg0[w] &= ~masks.site[w];
	}
	return cache_.emplace(key, std::move(masks)).first->second;
}

std::unique_ptr<PrePassGeometry> PrePassGeometry::LoadForPart(
    const std::string& db_root,
    const std::string& part,
    std::string* error) {
	// prjxray/util.py:get_fabric_for_part, in C++: parts.yaml gives the
	// part's device, devices.yaml gives the device's fabric, and the fabric
	// is the directory that carries tilegrid.json. The part directory
	// (xc7a100tcsg324-1) and the fabric directory (xc7a100t) are different
	// directories and only one of them holds part.yaml.
	const std::string parts_path = db_root + "/mapping/parts.yaml";
	const std::string devices_path = db_root + "/mapping/devices.yaml";
	std::string device, fabric;
	try {
		YAML::Node parts = YAML::LoadFile(parts_path);
		YAML::Node entry = parts[part];
		if (!entry || !entry["device"]) {
			if (error)
				*error = "part '" + part + "' is not in " +
				         parts_path;
			return nullptr;
		}
		device = entry["device"].as<std::string>();
		YAML::Node devices = YAML::LoadFile(devices_path);
		YAML::Node dev = devices[device];
		if (!dev || !dev["fabric"]) {
			if (error)
				*error = "device '" + device +
				         "' is not in " + devices_path;
			return nullptr;
		}
		fabric = dev["fabric"].as<std::string>();
	} catch (const std::exception& e) {
		if (error)
			*error = std::string("cannot resolve the fabric for '") +
			         part + "' through " + parts_path + " and " +
			         devices_path + ": " + e.what();
		return nullptr;
	}
	return Load(db_root + "/" + fabric + "/tilegrid.json", db_root, error);
}

std::unique_ptr<PrePassGeometry> PrePassGeometry::Load(
    const std::string& tilegrid_path,
    const std::string& segbits_dir,
    std::string* error) {
	std::unique_ptr<PrePassGeometry> geo(new PrePassGeometry());

	YAML::Node grid;
	try {
		// tilegrid.json is JSON, and JSON is YAML: yaml-cpp is already
		// libprjxray's parser for part.yaml, so this adds a file to
		// read and not a dependency. 12 MB / 30,932 tiles for
		// xc7a100t costs ~0.7 s and ~370 MB, once, and only when the
		// pre-pass is asked for.
		grid = YAML::LoadFile(tilegrid_path);
	} catch (const std::exception& e) {
		if (error)
			*error = "cannot read " + tilegrid_path + ": " +
			         e.what();
		return nullptr;
	}
	if (!grid.IsMap()) {
		if (error)
			*error = tilegrid_path + " is not a tile map.";
		return nullptr;
	}

	size_t tiles = 0;
	std::map<Kind, size_t> by_kind;
	for (const auto& kv : grid) {
		const YAML::Node& tile = kv.second;
		if (!tile.IsMap() || !tile["type"] || !tile["bits"])
			continue;
		const YAML::Node& bits = tile["bits"]["CLB_IO_CLK"];
		if (!bits || !bits["baseaddr"])
			continue;
		const std::string type = tile["type"].as<std::string>();

		auto it = geo->types_.find(type);
		if (it == geo->types_.end()) {
			TileType t;
			t.name = type;
			if (StartsWith(type, "CLBLL_") ||
			    StartsWith(type, "CLBLM_")) {
				t.kind = Kind::kClb;
			} else if (StartsWith(type, "DSP_")) {
				t.kind = Kind::kDsp;
			} else if (type == "BRAM_L" || type == "BRAM_R") {
				t.kind = Kind::kBram;
			} else if (StartsWith(type, "INT_") || type == "INT" ||
			           StartsWith(type, "HCLK")) {
				// Named by section 5.1 and owns nothing.
				t.kind = Kind::kInterconnect;
			} else {
				t.kind = Kind::kUnclassified;
			}
			it = geo->types_.emplace(type, std::move(t)).first;
		}

		uint32_t baseaddr = 0;
		try {
			baseaddr = static_cast<uint32_t>(std::stoul(
			    bits["baseaddr"].as<std::string>(), nullptr, 0));
		} catch (const std::exception&) {
			if (error)
				*error = "bad baseaddr on tile " +
				         kv.first.as<std::string>() + " in " +
				         tilegrid_path;
			return nullptr;
		}
		if ((baseaddr & kMinorMask) != 0) {
			if (error)
				*error = "tile " + kv.first.as<std::string>() +
				         " has a baseaddr with a nonzero minor";
			return nullptr;
		}
		TileSpan span;
		span.type = &it->second;
		span.offset = bits["offset"] ? bits["offset"].as<uint16_t>() : 0;
		span.words = bits["words"] ? bits["words"].as<uint16_t>() : 0;
		Column& column = geo->columns_[baseaddr];
		column.spans.push_back(span);
		if (it->second.kind == Kind::kUnclassified) {
			column.unclassified.insert(type);
			geo->unclassified_device_wide_[type]++;
		}
		by_kind[it->second.kind]++;
		++tiles;
	}

	// The excluded bit positions, from prjxray's own segbits rather than
	// from a table transcribed out of the verdict. Only the kinds that keep
	// anything need one.
	for (auto& entry : geo->types_) {
		TileType& t = entry.second;
		if (t.kind != Kind::kClb && t.kind != Kind::kDsp)
			continue;
		const std::string path =
		    segbits_dir + "/segbits_" + Lower(t.name) + ".db";
		auto reader = prjxray::SegbitsFileReader::InitWithFile(path);
		if (!reader) {
			if (error)
				*error =
				    "cannot read " + path +
				    ", which the per-tile SITE mask needs for " +
				    t.name +
				    ". The mask's excluded positions come from "
				    "prjxray's database, not from a constant.";
			return nullptr;
		}
		for (auto line = reader->begin(); line != reader->end();
		     ++line) {
			const std::string tag(line->tag());
			if (tag.empty())
				continue;
			const bool is_cin = EndsWith(tag, ".PRECYINIT.CIN");
			const bool is_mux =
			    tag.find(".DSP_GND_") != std::string::npos ||
			    tag.find(".DSP_VCC_") != std::string::npos;
			const bool is_reg0 = EndsWith(tag, ".AREG_0") ||
			                     EndsWith(tag, ".BREG_0");
			if (t.kind == Kind::kClb && !is_cin)
				continue;
			if (t.kind == Kind::kDsp && !is_mux && !is_reg0)
				continue;
			std::istringstream bits_in{std::string(line->bit())};
			std::string token;
			while (bits_in >> token) {
				// A leading '!' is a "must be zero" position:
				// it is not one of the feature's own bits and
				// so is not one of the bits pass 1 withholds.
				if (token.empty() || token[0] == '!')
					continue;
				Position p;
				if (!ParsePosition(token, &p))
					continue;
				if (is_reg0 && p.minor != 27)
					continue;
				AddPosition(&t.exclude, p);
				// INFERRED, section 5.1: the _X1 half of the
				// CIN exclusion has never been seen in a
				// bitstream -- all 66 measured instances are
				// SLICE*_X0 -- and is taken from prjxray by
				// symmetry with _X0.
				if (is_cin &&
				    tag.find("_X1.") != std::string::npos)
					AddPosition(&t.exclude_inferred_cin, p);
				// INFERRED, section 5.1: prjxray names these
				// only as one bit of a three-bit {A,B}REG code,
				// so "they are the DSP's cascade selector" is
				// a fit to the diff rather than a reading of
				// the database.
				if (is_reg0)
					AddPosition(&t.exclude_inferred_dsp, p);
			}
		}

		// Two literal cross-checks. Section 5.1 names these positions
		// as measured values; if the database no longer agrees with
		// the measurement, the measurement is what stops applying, and
		// a writer that carries on is emitting a mask nobody has ever
		// checked.
		if (t.kind == Kind::kClb) {
			const std::string got = PositionsToString(t.exclude);
			const std::string inferred =
			    PositionsToString(t.exclude_inferred_cin);
			if (got != "30_13 31_12" || inferred != "31_12") {
				if (error)
					*error =
					    "the PRECYINIT.CIN positions of " +
					    t.name + " in " + path + " are '" +
					    got +
					    "', and section 5.1 of "
					    "docs/far-double-write-verdict.md "
					    "measured '30_13 31_12'. Refusing "
					    "to apply a mask to a database "
					    "that has moved under it.";
				return nullptr;
			}
		}
		if (t.kind == Kind::kDsp) {
			const std::string got =
			    PositionsToString(t.exclude_inferred_dsp);
			if (got != "27_38 27_111 27_198 27_271") {
				if (error)
					*error =
					    "the {A,B}REG_0 minor-27 positions "
					    "of " +
					    t.name + " in " + path + " are '" +
					    got +
					    "', and section 5.1 measured "
					    "'27_38 27_111 27_198 27_271'.";
				return nullptr;
			}
			size_t total = 0;
			for (const auto& m : t.exclude)
				for (const auto& w : m.second)
					total += __builtin_popcount(w.second);
			// The four {A,B}REG_0 bits are counted in `total`; what
			// is left is the DSP_GND_*/DSP_VCC_* input-mux set,
			// which is 156 positions in the pinned database.
			const size_t mux = total - 4;
			if (mux < 8) {
				if (error)
					*error =
					    "found only " +
					    std::to_string(mux) +
					    " excluded positions for " +
					    t.name + " in " + path +
					    "; the DSP_GND_*/DSP_VCC_* input "
					    "muxes are missing, so the mask "
					    "would keep bits Vivado withholds.";
				return nullptr;
			}
		}
	}

	std::ostringstream s;
	s << "pre-pass geometry: " << tiles << " block-0 tile span(s) in "
	  << geo->columns_.size() << " configuration column(s) from "
	  << tilegrid_path << "; CLB " << by_kind[Kind::kClb] << ", BRAM "
	  << by_kind[Kind::kBram] << ", DSP " << by_kind[Kind::kDsp]
	  << ", INT/HCLK " << by_kind[Kind::kInterconnect] << ", unclassified "
	  << by_kind[Kind::kUnclassified] << " ("
	  << geo->unclassified_device_wide_.size() << " type(s))";
	geo->summary_ = s.str();
	return geo;
}

}  // namespace xc7series
}  // namespace xilinx
}  // namespace prjxray
