"""gen_stock_icons: stock page-row bitmaps -> upright row-major icons for the cgdn1_display driver."""
import os
import struct
from pathlib import Path

import pytest

from gen_stock_icons import ICONS, extract, header, load_segments, pack, read, upright

# The stock image is not distributed: point CGDN1_STOCK_IMAGE at your own copy to run the image tests.
STOCK = [Path(os.environ["CGDN1_STOCK_IMAGE"])] if os.environ.get("CGDN1_STOCK_IMAGE") else []
HEADER = Path(__file__).resolve().parent.parent / "components/cgdn1_display/stock_icons.h"


def test_upright_orientation():
    # One page row (w=3 bytes), h=8: GUI pixel (X=2, Y=1) -> upright row 0 (= W-1-X), col 1 (= Y).
    raw = bytes([0x00, 0x00, 0x02])
    rows = upright(raw, 3, 8)
    assert (len(rows), len(rows[0])) == (3, 8)
    assert [(r, c) for r in range(3) for c in range(8) if rows[r][c]] == [(0, 1)]


def test_pack_msb_first_padded():
    assert pack([[1, 0, 0, 0, 0, 0, 0, 0, 1]]) == bytes([0x80, 0x80])


def test_segments_and_read():
    img = bytearray([0xE9, 1]) + bytes(22) + struct.pack("<II", 0x3F400000, 4) + b"abcd"
    segs = load_segments(bytes(img))
    assert segs == [(0x3F400000, 4, 32)]
    assert read(bytes(img), segs, 0x3F400001, 2) == b"bc"
    with pytest.raises(ValueError):
        read(bytes(img), segs, 0x3F400003, 2)
    with pytest.raises(ValueError):
        load_segments(b"\0" * 32)


def test_committed_header_matches_stock_image():
    if not STOCK:
        pytest.skip("CGDN1_STOCK_IMAGE not set (stock 4.8.5 app image)")
    icons = extract(STOCK[0].read_bytes())
    assert [n for n, _, _ in icons] == [n for n, *_ in ICONS]
    assert HEADER.read_text() == header(icons, STOCK[0].name)


def _stock_icons():
    if not STOCK:
        pytest.skip("CGDN1_STOCK_IMAGE not set (stock 4.8.5 app image)")
    return {n: rows for n, _, rows in extract(STOCK[0].read_bytes())}


def test_page_assets_dimensions():
    icons = _stock_icons()
    want = {"HUGE_0": (32, 56), "HUGE_1": (16, 56), "HUGE_DOT": (8, 8), "PERCENT": (16, 15),
            "E2": (72, 55), "LOADER_0": (40, 11), "PAGE_1": (32, 4)}
    assert {n: (len(icons[n][0]), len(icons[n])) for n in want} == want


def test_page_dot_1_pattern():
    rows = ["".join("#" if v else "." for v in r) for r in _stock_icons()["PAGE_1"]]
    assert rows == [".####...........................",
                    ".#..#...##....##....##....##....",
                    ".#..#...##....##....##....##....",
                    ".####..........................."]


def test_header_has_groups():
    text = HEADER.read_text()
    assert "HUGE_DIGITS[10] = {&ICON_HUGE_0," in text
    assert "PAGE_DOTS[5] = {&ICON_PAGE_1," in text
