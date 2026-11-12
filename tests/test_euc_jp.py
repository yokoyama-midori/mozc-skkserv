"""Checks the EUC-JP table of mozc_skkserv (overlay/mozc_inproc/euc_jp.h).

Python's euc_jp codec is the reference. This script writes the cases (all
6,879 characters of JIS X 0208, ASCII, half-width katakana, the six
characters that Windows uses for the same cells, and many inputs that must
fail, including ill-formed UTF-8) and runs euc_jp_check.exe with them. Text is
written as hex of its UTF-8 bytes.

usage: python tests/test_euc_jp.py <path of euc_jp_check.exe>
"""
import pathlib
import subprocess
import sys
import tempfile


def u8(text):
    """The UTF-8 bytes of a text in hex."""
    return text.encode("utf-8").hex().upper()


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    cases = []

    # JIS X 0208 (the reference: Python's euc_jp) and the cells that must fail.
    valid_cells = 0
    for row in range(1, 95):
        for col in range(1, 95):
            data = bytes([0xA0 + row, 0xA0 + col])
            try:
                text = data.decode("euc_jp")
            except UnicodeDecodeError:
                text = None
            if (text is not None and (row <= 8 or 16 <= row <= 84)
                    and len(text) == 1 and ord(text) < 0x10000):
                assert text.encode("euc_jp") == data
                cases.append(f"ok\t{data.hex().upper()}\t{u8(text)}")
                valid_cells += 1
            else:
                # Includes rows 9-15 and 85-94 (NEC and IBM extensions, user-defined).
                cases.append(f"dec_fail\t{data.hex().upper()}")
    assert valid_cells == 6879, valid_cells

    # ASCII and half-width katakana.
    for b in range(0x20, 0x7F):
        cases.append(f"ok\t{b:02X}\t{u8(chr(b))}")
    for b in range(0xA1, 0xE0):
        cases.append(f"ok\t8E{b:02X}\t{u8(chr(0xFF61 + b - 0xA1))}")

    # Mixed text.
    for text in ["かんじ", "ABC漢字ｶﾅ!", "ー〜", "ｱｲｳｴｵ", "日本語 text"]:
        cases.append(f"ok\t{text.encode('euc_jp').hex().upper()}\t{u8(text)}")

    # The six characters that Windows uses for cells of JIS X 0208.
    for cp, cell in [(0xFF5E, "A1C1"), (0x2225, "A1C2"), (0xFF0D, "A1DD"),
                     (0xFFE0, "A1F1"), (0xFFE1, "A1F2"), (0xFFE2, "A2CC")]:
        cases.append(f"encode\t{u8(chr(cp))}\t{cell}")
    # Standard characters of the same cells (decoded as such).
    for cell, cp in [("A1C1", 0x301C), ("A1C2", 0x2016), ("A1DD", 0x2212),
                     ("A1F1", 0xA2), ("A1F2", 0xA3), ("A2CC", 0xAC)]:
        cases.append(f"ok\t{cell}\t{u8(chr(cp))}")

    # Inputs that are not valid or not supported.
    for hexbytes in ["00", "0A", "1F", "7F", "80", "8E", "8EA0", "8EE0", "8EFF",
                     "A1", "A1A0", "A1FF", "FFA1", "8FA2AF", "8F", "8FA1", "A4",
                     "A4A2A4", "ADA1", "F9A1", "FEFE"]:
        cases.append(f"dec_fail\t{hexbytes}")

    # Characters that cannot be represented.
    for code in [0x00, 0x0A, 0x1F, 0x7F, 0xA5, 0xE9, 0x2014, 0x2460, 0x1F600, 0x10000,
                 0xFFFD, 0x20AC, 0x3042 + 0x10000]:
        cases.append(f"enc_fail\t{u8(chr(code))}")
    # One bad character fails the whole text.
    cases.append(f"enc_fail\t{u8('あ😀い')}")

    # Ill-formed UTF-8: the decoder of Mozc gives U+FFFD, which cannot be
    # represented, so Encode() fails (the server never sends such a text).
    for hexbytes in ["E3", "E381", "FF", "80", "C080", "E08181", "EDA080",
                     "F4908080", "F888808080", "E38182E3", "41FF42"]:
        cases.append(f"enc_fail\t{hexbytes}")

    path = pathlib.Path(tempfile.gettempdir()) / "euc_jp_cases.tsv"
    path.write_text("\n".join(cases) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(cases)} cases -> {path}")
    sys.exit(subprocess.run([sys.argv[1], str(path)]).returncode)


if __name__ == "__main__":
    main()
