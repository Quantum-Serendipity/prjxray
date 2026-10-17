#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
import fasm
import sys

from prjxray import bitstream
from prjxray import cross_tile_features


class FasmLookupError(Exception):
    pass


class FasmInconsistentBits(Exception):
    pass


class FasmFeatureConflict(Exception):
    """Raised when two FASM lines assign different values to the same
    feature address."""
    pass


class FasmTileWindowViolation(Exception):
    """Raised when a feature resolves to a bit outside its own tile's frames
    or words, i.e. a bit that belongs to another tile."""
    pass


class FasmCrossTileWrite(FasmTileWindowViolation):
    pass


class FasmRoiViolation(Exception):
    """Raised in strict-ROI mode when the emitted frame set would leave the ROI.

    Two distinct conditions raise this, and both mean the same thing to the
    caller -- the frame set you are about to write is not the frame set you
    asked for:

      * a FASM feature names a tile outside the ROI rectangle, or
      * the emitted frame address set is not exactly the ROI's frame set.

    The second is a backstop for the first: it holds regardless of how the
    features got in (input file, part-level required_features, PUDC_B, the
    STEPDOWN fixups in fasm2frames), so it cannot be bypassed by adding a new
    injection site.
    """
    pass


# How many out-of-ROI TILES to name individually before summarising.  A
# whole-device FASM against a small ROI produces thousands of them and an
# unbounded list is not a diagnostic.
_ROI_VIOLATION_REPORT_LIMIT = 20


def bit_in_tile_window(bits, bit):
    word = bit.word_bit // bitstream.WORD_SIZE_BITS
    return (
        bits.base_address <= bit.word_column < bits.base_address + bits.frames
        and bits.offset <= word < bits.offset + bits.words)


def check_bit_in_tile_window(
        tile, bits, block_type, bit, line, allowed_hosts=(), landing=()):
    if not bit.isset or bit_in_tile_window(bits, bit):
        return

    word = bit.word_bit // bitstream.WORD_SIZE_BITS
    raise (FasmCrossTileWrite if landing else FasmTileWindowViolation)(
        "FASM line '{}' resolves to frame 0x{:08X} word {} bit {}, outside "
        "tile {}'s own {} window (frames 0x{:08X}..0x{:08X}, words {}..{}){}; "
        "{}".format(
            line, bit.word_column, word, bit.word_bit %
            bitstream.WORD_SIZE_BITS, tile, block_type.value
            if hasattr(block_type, 'value') else block_type,
            bits.base_address, bits.base_address + bits.frames - 1,
            bits.offset, bits.offset + bits.words - 1,
            ' and outside every tile of the type(s) it is declared to cross '
            'into ({})'.format(', '.join(allowed_hosts))
            if allowed_hosts else '',
            'the bit would land in tile {}'.format(', '.join(landing))
            if landing else
            'the bit lands in no tile: its word is past the end of the frame '
            'or in words no tile owns'))


def feature_assignment(set_feature):
    '''Return (mask, value) over the feature's addresses: mask has a bit for
    every address the line assigns, value the bit assigned there.'''
    if set_feature.start is None:
        return 1, 1 if set_feature.value else 0

    if set_feature.end is None:
        return 1 << set_feature.start, (
            1 << set_feature.start if set_feature.value else 0)

    width = set_feature.end - set_feature.start + 1
    width_mask = (1 << width) - 1
    return width_mask << set_feature.start, (
        set_feature.value & width_mask) << set_feature.start


def set_feature_as_written(set_feature):
    '''Render set_feature with its value zero-padded to the feature width,
    the form FASM writers emit, rather than fasm's minimal-digit form.'''
    text = fasm.set_feature_to_str(set_feature)
    width = fasm.set_feature_width(set_feature)
    value_format = set_feature.value_format
    if value_format == fasm.ValueFormat.VERILOG_BINARY:
        digits = '{:0{}b}'.format(set_feature.value, width)
        radix = 'b'
    elif value_format == fasm.ValueFormat.VERILOG_HEX:
        digits = '{:0{}X}'.format(set_feature.value, (width + 3) // 4)
        radix = 'h'
    elif value_format == fasm.ValueFormat.VERILOG_OCTAL:
        digits = '{:0{}o}'.format(set_feature.value, (width + 2) // 3)
        radix = 'o'
    else:
        return text

    return "{} = {}'{}{}".format(
        text.rsplit(' = ', 1)[0], width, radix, digits)


def init_frame_at_address(frames, addr):
    '''Set given frame to 0 if not initialized '''
    if not addr in frames:
        frames[addr] = [0 for _i in range(bitstream.FRAME_WORD_COUNT)]


class FasmAssembler(object):
    def __init__(self, db):
        self.db = db
        self.grid = db.grid()

        self.seen_tile = set()
        self.frames_in_use = set()

        self.frames = {}
        self.frames_line = {}

        self.feature_assignments = {}

        self.feature_callback = lambda feature: None

        # Strict-ROI state.  self.roi is None unless mark_roi_frames() was
        # called with strict=True; everything guarded by it is inert in the
        # default permissive mode.
        self.roi = None
        self.roi_frames = None
        # (tile, feature) -> the FASM line that named it.  A dict, not a list,
        # because a single out-of-ROI tile typically contributes dozens of
        # features and repeating the same tile dozens of times buries the
        # others.
        self.roi_violations = {}

    def set_feature_callback(self, feature_callback):
        self.feature_callback = feature_callback

    def check_roi(self):
        '''Raise if strict-ROI mode was requested and was violated.

        Idempotent and cheap; call it as often as convenient.
        '''
        if not self.roi_violations:
            return

        roi = self.roi

        # Group by tile.  One out-of-ROI CLB contributes ~30 features, so a
        # flat list of the first 20 would report one tile 20 times and hide the
        # other 79.  Per tile: where it is, how many of its features were
        # rejected, and one of them named in full so the FASM line is greppable.
        by_tile = {}
        for (tile, feature), (loc, tile_type, line) in \
                self.roi_violations.items():
            entry = by_tile.get(tile)
            if entry is None:
                by_tile[tile] = [loc, tile_type, 1, feature, line]
            else:
                entry[2] += 1
                if feature < entry[3]:
                    entry[3], entry[4] = feature, line

        lines = [
            'FASM features outside the ROI (GRID_X {}..{}, GRID_Y {}..{}) '
            'with --roi-strict:'.format(roi.x1, roi.x2, roi.y1, roi.y2)
        ]
        for tile in sorted(by_tile)[:_ROI_VIOLATION_REPORT_LIMIT]:
            loc, tile_type, count, feature, line = by_tile[tile]
            lines.append(
                "  {} (type {}, grid_x={}, grid_y={}): {} out-of-ROI "
                "feature(s), e.g. '{}' from FASM line '{}'".format(
                    tile, tile_type, loc.grid_x, loc.grid_y, count, feature,
                    line))
        if len(by_tile) > _ROI_VIOLATION_REPORT_LIMIT:
            lines.append(
                '  ... and {} more out-of-ROI tile(s)'.format(
                    len(by_tile) - _ROI_VIOLATION_REPORT_LIMIT))
        lines.append(
            '{} out-of-ROI feature(s) over {} distinct tile(s). Each one would '
            'have pulled that tile\'s whole frame column into the emitted set '
            '-- see --roi vs --roi-strict in fasm2frames --help.'.format(
                len(self.roi_violations), len(by_tile)))
        raise FasmRoiViolation('\n'.join(lines))

    def get_frames(self, sparse=False):
        # Unbypassable: no caller gets frames out of an assembler whose FASM
        # left the ROI, however it called us.
        self.check_roi()

        if not sparse:
            frames = self.frames_init()
        else:
            # Even in sparse mode, zero all frames for any tile that is
            # setting a bit.  This handles the case where the tile has
            # multiple frames, but the FASM only specifies some of the frames.
            frames = {}
            for frame in self.frames_in_use:
                init_frame_at_address(frames, frame)

        for (frame_addr, word_addr, bit_index), is_set in self.frames.items():
            init_frame_at_address(frames, frame_addr)

            if word_addr >= 101:
                print(f"get_frames: invalid word address {word_addr} in line: {self.frames_line[(frame_addr, word_addr, bit_index)]}", file=sys.stderr)
            if frame_addr not in frames:
                print(f"get_frames: invalid frame address {frame_addr:x8}", file=sys.stderr)
            if is_set:
                frames[frame_addr][word_addr] |= 1 << bit_index

        # The one assertion that retires the union defect: in strict mode the
        # emitted address set must be EXACTLY the ROI's, not a superset of it.
        # The per-feature check above is the diagnosis; this is the proof.  It
        # also covers the case the per-feature check cannot see -- an in-ROI
        # tile whose segbits reach a frame column the ROI does not own.
        if self.roi_frames is not None:
            emitted = set(frames)
            extra = emitted - self.roi_frames
            missing = self.roi_frames - emitted
            if extra or missing:
                raise FasmRoiViolation(
                    'strict ROI frame-set mismatch: emitted {} frames, ROI '
                    'expects {}.\n'
                    '  {} frame(s) outside the ROI, first: {}\n'
                    '  {} ROI frame(s) not emitted, first: {}'.format(
                        len(emitted), len(self.roi_frames), len(extra),
                        ', '.join('0x%08X' % a for a in sorted(extra)[:8])
                        or '(none)', len(missing), ', '.join(
                            '0x%08X' % a
                            for a in sorted(missing)[:8]) or '(none)'))

        return frames

    def frames_init(self):
        '''Set all frames to 0'''
        frames = {}

        for bits_info in self.grid.iter_all_frames():
            for coli in range(bits_info.bits.frames):
                init_frame_at_address(
                    frames, bits_info.bits.base_address + coli)

        return frames

    def frame_set(self, frame_addr, word_addr, bit_index, line):
        '''Set given bit in given frame address and word'''
        assert bit_index is not None

        key = (frame_addr, word_addr, bit_index)
        if word_addr >= 101:
            print(f"frame_set: invalid word address {word_addr} in line: {line}", file=sys.stderr)
            return
        if key in self.frames:
            if self.frames[key] != 1:
                raise FasmInconsistentBits(
                    'FASM line "{}" wanted to set bit {} but was cleared by FASM line "{}"'
                    .format(
                        line,
                        key,
                        self.frames_line[key],
                    ))
            return

        self.frames[key] = 1
        self.frames_line[key] = line

    def frame_clear(self, frame_addr, word_addr, bit_index, line):
        '''Set given bit in given frame address and word'''
        assert bit_index is not None

        key = (frame_addr, word_addr, bit_index)
        if word_addr >= 101:
            print(f"frame_clear: invalid word address {word_addr} in line: {line}", file=sys.stderr)
            return
        if key in self.frames:
            if self.frames[key] != 0:
                raise FasmInconsistentBits(
                    'FASM line "{}" wanted to clear bit {} but was set by FASM line "{}"'
                    .format(
                        line,
                        key,
                        self.frames_line[key],
                    ))
            return

        self.frames[key] = 0
        self.frames_line[key] = line

    def enable_feature(self, tile, feature, address, line):
        gridinfo = self.grid.gridinfo_at_tilename(tile)

        if self.roi is not None:
            loc = self.grid.loc_of_tilename(tile)
            if not self.roi.tile_in_roi(loc):
                # Record and return WITHOUT touching self.frames or
                # self.frames_in_use.  check_roi() will turn this into a
                # non-zero exit, but returning early also means that even a
                # caller that swallows the exception cannot end up with the
                # tile's 36-frame column in the emitted set.
                self.roi_violations[(tile, feature)] = (
                    loc, gridinfo.tile_type, line)
                return

        def update_segbit(bit):
            '''Set or clear a single bit in a segment at the given word column and word bit position'''

            frame_addr = bit.word_column
            word_addr = bit.word_bit // bitstream.WORD_SIZE_BITS
            bit_index = bit.word_bit % bitstream.WORD_SIZE_BITS

            if bit.isset:
                self.frame_set(frame_addr, word_addr, bit_index, line)
            else:
                self.frame_clear(frame_addr, word_addr, bit_index, line)

        segbits = self.grid.get_tile_segbits_at_tilename(tile)

        self.seen_tile.add(tile)

        db_k = '%s.%s' % (gridinfo.tile_type, feature)

        any_bits = set()

        try:
            feature_bits = list(
                segbits.feature_to_bits(gridinfo.bits, db_k, address))
        except KeyError:
            raise FasmLookupError(
                "Segment DB %s, key %s not found from line '%s'" %
                (gridinfo.tile_type, db_k, line))

        allowed_hosts = cross_tile_features.ALLOWED.get(
            (gridinfo.tile_type, feature), ())
        written = []
        host_bits = []
        for block_type, bit in feature_bits:
            bits = gridinfo.bits[block_type]
            if bit_in_tile_window(bits, bit):
                written.append(bit)
            elif bit.isset:
                host = self.cross_tile_host(allowed_hosts, block_type, bit)
                if host is None:
                    check_bit_in_tile_window(
                        tile, bits, block_type, bit, line, allowed_hosts,
                        self.tiles_landed_in(tile, block_type, bit))
                host_bits.append(self.grid.gridinfo_at_tilename(host).bits[
                    block_type])
                written.append(bit)
            any_bits.add(block_type)

        for bit in written:
            update_segbit(bit)

        in_use = [gridinfo.bits[block_type] for block_type in any_bits]
        for bits in in_use + host_bits:
            for frame in range(bits.base_address,
                               bits.base_address + bits.frames):
                self.frames_in_use.add(frame)

    def tiles_landed_in(self, tile, block_type, bit):
        return [
            '{} ({})'.format(
                other, self.grid.gridinfo_at_tilename(other).tile_type)
            for other in self.grid.tiles_containing(
                block_type, bit.word_column,
                bit.word_bit // bitstream.WORD_SIZE_BITS) if other != tile
        ]

    def cross_tile_host(self, allowed_hosts, block_type, bit):
        if not allowed_hosts:
            return None
        for host in self.grid.tiles_containing(
                block_type, bit.word_column,
                bit.word_bit // bitstream.WORD_SIZE_BITS):
            if self.grid.gridinfo_at_tilename(
                    host).tile_type in allowed_hosts:
                return host
        return None

    def record_feature_assignment(self, set_feature):
        mask, value = feature_assignment(set_feature)
        prior = self.feature_assignments.get(set_feature.feature)
        if prior is None:
            self.feature_assignments[set_feature.feature] = [
                mask, value, [set_feature]
            ]
            return

        prior_mask, prior_value, prior_set_features = prior
        conflict = (prior_value ^ value) & prior_mask & mask
        if conflict:
            addresses = [
                address for address in range(conflict.bit_length())
                if conflict & (1 << address)
            ]
            earlier = []
            for prior_set_feature in prior_set_features:
                earlier_mask, earlier_value = feature_assignment(
                    prior_set_feature)
                if (earlier_value ^ value) & earlier_mask & mask:
                    earlier.append(
                        '"{}"'.format(
                            set_feature_as_written(prior_set_feature)))
            raise FasmFeatureConflict(
                'FASM line "{}" assigns {} a value that FASM line {} '
                'already assigned differently, at address(es) {}{}'.format(
                    set_feature_as_written(set_feature), set_feature.feature,
                    ' and '.join(earlier), ', '.join(
                        str(a) for a in addresses[:8]),
                    ' and {} more'.format(len(addresses) - 8)
                    if len(addresses) > 8 else ''))

        if (prior_mask | mask) != prior_mask:
            prior_set_features.append(set_feature)
        prior[0] = prior_mask | mask
        prior[1] = prior_value | value

    def add_fasm_line(self, line, missing_features):
        if not line.set_feature:
            return

        self.feature_callback(line.set_feature)

        line_strs = tuple(fasm.fasm_line_to_string(line))
        assert len(line_strs) == 1
        line_str = line_strs[0]

        self.record_feature_assignment(line.set_feature)

        parts = line.set_feature.feature.split('.')
        tile = parts[0]
        feature = '.'.join(parts[1:])

        # canonical_features flattens multibit feature enables to only
        # single bit features, which is what enable_feature expects.
        #
        # canonical_features also filters out features that are not enabled,
        # which are no-ops.
        for flat_set_feature in fasm.canonical_features(line.set_feature):
            address = 0
            if flat_set_feature.start is not None:
                address = flat_set_feature.start

            try:
                self.enable_feature(tile, feature, address, line_str)
            except FasmLookupError as e:
                missing_features.append(str(e))

    def parse_fasm_filename(self, filename, extra_features=[]):
        missing_features = []
        for line in fasm.parse_fasm_filename(filename):
            self.add_fasm_line(line, missing_features)

        for line in extra_features:
            self.add_fasm_line(line, missing_features)

        self.check_roi()

        if missing_features:
            # Allow the assembler to keep going when only a small number of
            # niche segbits (e.g. IOB_Y1.IBUF_HP_BANK_GLUE — not yet fuzzed
            # for the slave side) are missing.  The resulting bitstream is
            # missing those bits — Vivado's silicon defaults usually cover
            # them — and the user gets a console warning per missing line.
            # Set XRAY_ALLOW_MISSING_FEATURES=1 to opt in.
            import os
            if os.environ.get("XRAY_ALLOW_MISSING_FEATURES") == "1":
                import sys
                print("WARNING: " + str(len(missing_features))
                        + " missing-feature lines suppressed by "
                        "XRAY_ALLOW_MISSING_FEATURES=1:", file=sys.stderr)
                for m in missing_features:
                    print("  " + m, file=sys.stderr)
                return
            raise FasmLookupError('\n'.join(missing_features))

    def mark_roi_frames(self, roi, strict=False):
        '''Seed frames_in_use with every frame the ROI's tiles occupy.

        strict=False (default) is the historical behaviour and is a UNION: the
        emitted frame set ends up as (ROI frames) | (frames of every tile any
        FASM feature touches).  That is correct when the device model handed to
        place-and-route has already been filtered to the region, because then
        no out-of-region tile exists to be dragged in.

        strict=True makes the ROI a FILTER instead: enable_feature() refuses
        any tile outside the rectangle, and get_frames() asserts that the
        emitted address set is exactly this one.  Required whenever the device
        model is the FULL device, where a stray feature silently grows the
        "partial" bitstream to cover static logic that writing it will destroy.
        '''
        roi_frames = set()
        for tile in roi.gen_tiles():
            gridinfo = self.grid.gridinfo_at_tilename(tile)

            for block_type in gridinfo.bits:
                bits = gridinfo.bits[block_type]
                for frame in range(bits.base_address,
                                   bits.base_address + bits.frames):
                    roi_frames.add(frame)

        if strict:
            sharing = []
            for tile in sorted(self.grid.tiles()):
                if roi.tile_in_roi(self.grid.loc_of_tilename(tile)):
                    continue
                gridinfo = self.grid.gridinfo_at_tilename(tile)
                if any(frame in roi_frames
                       for bits in gridinfo.bits.values()
                       for frame in range(
                           bits.base_address, bits.base_address +
                           bits.frames)):
                    sharing.append(
                        '{} (type {})'.format(tile, gridinfo.tile_type))
            if sharing:
                raise FasmRoiViolation(
                    '--roi-strict ROI (GRID_X {}..{}, GRID_Y {}..{}) shares '
                    'configuration frames with {} tile(s) outside it, whose '
                    'words in those frames a sparse write would zero: {}{}. '
                    'A frame spans a whole clock-region row of its column; '
                    'widen the ROI to whole rows of the columns it '
                    'touches.'.format(
                        roi.x1, roi.x2, roi.y1, roi.y2, len(sharing),
                        ', '.join(sharing[:_ROI_VIOLATION_REPORT_LIMIT]),
                        ', ...' if
                        len(sharing) > _ROI_VIOLATION_REPORT_LIMIT else ''))

        self.frames_in_use |= roi_frames
        if strict:
            self.roi = roi
            self.roi_frames = frozenset(roi_frames)
