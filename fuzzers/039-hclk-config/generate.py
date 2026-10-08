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

import json

from prjxray.segmaker import Segmaker


def bitfilter(frame, bit):
    return True


def main():
    segmk = Segmaker("design.bits")

    print("Loading tags")
    with open('params.json') as f:
        params = json.load(f)

    placed = {}
    try:
        with open('bufio_sites.txt') as f:
            for line in f:
                parts = line.split()
                if len(parts) == 2:
                    placed[parts[0]] = parts[1]
    except FileNotFoundError:
        pass
    used_sites = set(placed.values())

    for row in params:
        if row.get('kind') == 'BUFIO_TILE':
            sites = row['bufio_sites']
            ys = sorted(int(st.split('Y')[-1]) for st in sites)
            y_min = ys[0] if ys else 0
            for st in sites:
                y = int(st.split('Y')[-1]) - y_min
                segmk.add_tile_tag(
                    row['tile'], 'BUFIO_Y{}.IN_USE'.format(y), 1 if st in used_sites else 0)
            continue
        base_name = 'BUFR_Y{}'.format(row['y'])
        # v5: regional-clock leaf into the IOI column, per RCLK index (slot rel-y -> RCLK {0:2,1:3,2:0,3:1})
        rclk = {0: 2, 1: 3, 2: 0, 3: 1}[row['y']]
        segmk.add_tile_tag(
            row['tile'], 'HCLK_IOI_RCLK2IO{}.HCLK_IOI_CK_BUFRCLK{}'.format(rclk, rclk),
            1 if (row['IN_USE'] and row.get('consumed')) else 0)

        segmk.add_tile_tag(
            row['tile'], '{}.IN_USE'.format(base_name), row['IN_USE'])

        if not row['IN_USE']:
            continue

        segmk.add_tile_tag(
            row['tile'], '{}.BUFR_DIVIDE.BYPASS'.format(base_name),
            '"BYPASS"' == row['BUFR_DIVIDE'])
        for opt in range(1, 9):
            if row['BUFR_DIVIDE'] == str(opt):
                segmk.add_tile_tag(
                    row['tile'], '{}.BUFR_DIVIDE.D{}'.format(base_name, opt),
                    1)
            elif '"BYPASS"' == row['BUFR_DIVIDE']:
                segmk.add_tile_tag(
                    row['tile'], '{}.BUFR_DIVIDE.D{}'.format(base_name, opt),
                    0)

    segmk.compile(bitfilter=bitfilter)
    segmk.write()


if __name__ == '__main__':
    main()
