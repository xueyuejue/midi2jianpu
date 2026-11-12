# -*- coding: utf-8 -*-
"""Regenerate src/legacy_tables.inc (GBK + JIS X 0208 -> Unicode).

MIDI text meta events are raw bytes with no encoding declaration. Old Chinese
and Japanese MIDI files commonly use GBK (CP936) or Shift-JIS (CP932). We ship
compact legacy tables so every build (MSVC / Emscripten) decodes identically
without platform APIs or third-party libraries.

Two tables:
  * kGb2312Hex  -- the GBK / GB2312 COMMON ZONE: lead 0xA1..0xFE, trail 0xA1..0xFE
                   (a 94x94 grid). Built with the "gbk" codec so the 33 cells that
                   GBK defines inside the GB2312 byte range but GB2312 leaves
                   undefined also resolve (e.g. U+6DFC 淼 at E9? no, ED B5).
  * kGbkExtHex   -- the GBK EXTENSION ZONES that the common 94x94 grid cannot hold:
                     - lead 0xA1..0xFE with trail 0x40..0x7E / 0x80..0xA0
                     - lead 0x81..0xA0 with any valid trail 0x40..0xFE (excl 0x7F)
                   This is where GBK rare characters live, e.g. 镕(E9 46),
                   喆(86 B4), 堃(88 D2).

Usage:  python tools/gen_legacy_tables.py
"""
import io
import os

ROWS = 94          # common-zone grid is 94x94 (lead/trail 0xA1..0xFE)
COLS = 94
# GBK trail space: 0x40..0xFE excluding 0x7F == 190 values.
GBK_TRAILS = [t for t in range(0x40, 0xFF) if t != 0x7F]
assert len(GBK_TRAILS) == 190, len(GBK_TRAILS)


def sjis_to_jis(s1, s2):
    """Shift-JIS double byte -> JIS X 0208 (row, col), 1-based."""
    if s1 <= 0x9F:
        row = (s1 - 0x81) * 2 + 1
    elif s1 >= 0xE0:
        row = (s1 - 0xE0) * 2 + 63
    else:
        return None
    if s2 < 0x9F:
        col = s2 - (0x40 if s2 < 0x80 else 0x41)
    elif s2 <= 0xFC:
        row += 1
        col = s2 - 0x9F
    else:
        return None
    col += 1
    if not (1 <= row <= ROWS and 1 <= col <= COLS):
        return None
    return row, col


def build_jis0208():
    """Invert the platform codec: enumerate every Shift-JIS double byte."""
    table = [0] * (ROWS * COLS)
    seen = set()
    for s1 in list(range(0x81, 0xA0)) + list(range(0xE0, 0xFD)):
        for s2 in list(range(0x40, 0x7F)) + list(range(0x80, 0xFD)):
            rc = sjis_to_jis(s1, s2)
            if rc is None:
                continue
            raw = bytes((s1, s2))
            try:
                ch = raw.decode("shift_jis")
            except UnicodeDecodeError:
                continue
            if len(ch) != 1:
                continue
            row, col = rc
            idx = (row - 1) * COLS + (col - 1)
            cp = ord(ch)
            # Round-trip check: the codec must agree on our indexing.
            try:
                if ch.encode("shift_jis") != raw:
                    continue
            except UnicodeEncodeError:
                continue
            if table[idx] not in (0, cp):
                raise SystemExit("JIS cell collision at row=%d col=%d" % (row, col))
            table[idx] = cp
            seen.add(raw)
    return table, len(seen)


def build_gb2312():
    """GBK common zone (lead A1..FE, trail A1..FE) -> 94x94 Unicode grid.

    Built with the "gbk" codec (not "gb2312") so the handful of cells GBK defines
    inside the GB2312 byte range but GB2312 leaves undefined also resolve.
    """
    table = [0] * (ROWS * COLS)
    n = 0
    for row in range(1, ROWS + 1):
        for col in range(1, COLS + 1):
            raw = bytes((0xA0 + row, 0xA0 + col))
            try:
                ch = raw.decode("gbk")
            except UnicodeDecodeError:
                continue
            if len(ch) != 1:
                continue
            cp = ord(ch)
            if cp == 0xFFFD:
                continue
            table[(row - 1) * COLS + (col - 1)] = cp
            n += 1
    return table, n


def gbk_ext_cell_index(lead, trail):
    """Flat offset into kGbkExtHex, or None if (lead, trail) is not an extension cell."""
    if 0x81 <= lead <= 0xA0:
        # zone 2: 32 leads x 190 trails
        rs = (lead - 0x81) * 190
        if 0x40 <= trail <= 0x7E:
            ti = trail - 0x40
        elif 0x80 <= trail <= 0xFE:
            ti = 63 + (trail - 0x80)
        else:
            return None
        return rs + ti
    if 0xA1 <= lead <= 0xFE:
        # zone 1: 94 leads x 96 trails (trail 0x40..0x7E / 0x80..0xA0)
        rs = 32 * 190 + (lead - 0xA1) * 96
        if 0x40 <= trail <= 0x7E:
            ti = trail - 0x40
        elif 0x80 <= trail <= 0xA0:
            ti = 63 + (trail - 0x80)
        else:
            return None
        return rs + ti
    return None


def build_gbk_ext():
    """GBK extension zones not representable in the 94x94 common grid."""
    size = 32 * 190 + 94 * 96
    table = [0] * size
    n = 0
    for lead in list(range(0x81, 0xA1)) + list(range(0xA1, 0xFF)):
        if lead <= 0xA0:
            trails = list(range(0x40, 0x80)) + list(range(0x80, 0xFF))   # 190
        else:
            trails = list(range(0x40, 0x80)) + list(range(0x80, 0xA1))   # 96
        for trail in trails:
            idx = gbk_ext_cell_index(lead, trail)
            raw = bytes((lead, trail))
            try:
                ch = raw.decode("gbk")
            except UnicodeDecodeError:
                continue
            if len(ch) != 1:
                continue
            cp = ord(ch)
            if cp == 0xFFFD:
                continue
            if table[idx] not in (0, cp):
                raise SystemExit("GBK ext collision at %02X %02X" % (lead, trail))
            table[idx] = cp
            n += 1
    return table, n


def emit(name, table, comment):
    cells = len(table)
    hexs = "".join("%04X" % (v & 0xFFFF) for v in table)
    assert len(hexs) == cells * 4, (len(hexs), cells * 4)
    lines = []
    for i in range(0, len(hexs), 96):
        lines.append('    "%s"' % hexs[i:i + 96])
    return ("// %s\n" % comment +
            "static const char* const %s =\n" % name +
            "\n".join(lines) + ";\n\n")


def main():
    gb, gbn = build_gb2312()
    ext, extn = build_gbk_ext()
    jis, jisn = build_jis0208()
    # Sanity: a few well-known characters must survive the inversion.
    assert gb[(0xB0 - 0xA0 - 1) * COLS + (0xA1 - 0xA0 - 1)] == 0x554A, "GBK 啊"
    assert gbk_ext_cell_index(0xE9, 0x46) is not None, "镕 lead/trail valid"
    assert ext[gbk_ext_cell_index(0xE9, 0x46)] == 0x9555, "镕 -> U+9555"
    assert ext[gbk_ext_cell_index(0x86, 0xB4)] == 0x5586, "喆 -> U+5586"
    assert ext[gbk_ext_cell_index(0x88, 0xD2)] == 0x5803, "堃 -> U+5803"
    assert gb[(0xED - 0xA0 - 1) * COLS + (0xB5 - 0xA0 - 1)] == 0x6DFC, "淼 -> U+6DFC"
    assert gb[(0xF6 - 0xA0 - 1) * COLS + (0xCE - 0xA0 - 1)] == 0x946B, "鑫 -> U+946B"
    assert sjis_to_jis(0x81, 0x40) == (1, 1), "sjis_to_jis(0x8140)"
    assert sjis_to_jis(0x82, 0xA0) == (4, 2), "sjis_to_jis(0x82A0)"
    jis_at = jis[(1 - 1) * COLS + (1 - 1)]
    assert jis_at == ord("\u3000"), hex(jis_at)

    body = ("// AUTO-GENERATED by tools/gen_legacy_tables.py -- DO NOT EDIT.\n"
            "// All tables are big-endian uint16 code points, 4 hex digits per cell,\n"
            "// 0 means \"undefined\".\n"
            "// kGb2312Hex: 94x94 grid, index = (row-1)*94+(col-1), row=lead-0xA0,\n"
            "//   col=trail-0xA0 (lead/trail in 0xA1..0xFE). Built with the gbk codec.\n"
            "// kGbkExtHex: flat GBK extension zones; index by gbkExtCell() in\n"
            "//   src/text_encoding.hpp (lead 0x81..0xA0 all trails / lead 0xA1..0xFE\n"
            "//   trail 0x40..0x7E,0x80..0xA0).\n"
            "// kJis0208Hex: 94x94, JIS X 0208 via Shift-JIS bytes.\n"
            "// Rebuild: python tools/gen_legacy_tables.py\n"
            "#pragma once\n\n")
    body += emit("kGb2312Hex", gb,
                 "GBK / GB2312 common zone (lead 0xA1..0xFE, trail 0xA1..0xFE). "
                 "%d defined cells (gbk codec)." % gbn)
    body += emit("kGbkExtHex", ext,
                 "GBK extension zones (lead 0x81..0xA0 all trails / lead 0xA1..0xFE "
                 "trail 0x40..0x7E,0x80..0xA0). %d defined cells." % extn)
    body += emit("kJis0208Hex", jis,
                 "JIS X 0208 via Shift-JIS bytes. %d double-byte sequences round-tripped." % jisn)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "legacy_tables.inc")
    out = os.path.normpath(out)
    with io.open(out, "w", encoding="ascii", newline="\r\n") as f:
        f.write(body)
    print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))
    print("GBK common cells: %d, GBK ext cells: %d, JIS round-trips: %d"
          % (gbn, extn, jisn))


if __name__ == "__main__":
    main()
