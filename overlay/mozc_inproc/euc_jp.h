// Copyright (c) 2026 mozc-skkserv contributors. BSD-3-Clause (see LICENSE).
//
// EUC-JP <-> UTF-8 for the SKK server. It does not use the code pages of the
// operating system or iconv, so it behaves the same on every platform.
//
// Supported: ASCII (0x20-0x7E), JIS X 0201 half-width katakana (8E xx) and the
// 6,879 characters of JIS X 0208. Not supported (they fail): control
// characters, JIS X 0212 (8F xx xx), the extensions of NEC and IBM and any
// invalid byte sequence.
//
// The table is the standard JIS mapping, so EUC-JP "A1 C1" is U+301C WAVE DASH.
// Windows uses other characters for six cells. When a text is converted to
// EUC-JP, these Windows characters are accepted for the same cell (the same as
// crvskkserv does); converting from EUC-JP always gives the standard one:
//
//   cell   standard   also accepted
//   A1 C1  U+301C     U+FF5E FULLWIDTH TILDE
//   A1 C2  U+2016     U+2225 PARALLEL TO
//   A1 DD  U+2212     U+FF0D FULLWIDTH HYPHEN-MINUS
//   A1 F1  U+00A2     U+FFE0 FULLWIDTH CENT SIGN
//   A1 F2  U+00A3     U+FFE1 FULLWIDTH POUND SIGN
//   A2 CC  U+00AC     U+FFE2 FULLWIDTH NOT SIGN
#ifndef MOZC_SKKSERV_EUC_JP_H_
#define MOZC_SKKSERV_EUC_JP_H_

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "mozc_inproc/jisx0208_table.h"

namespace mozc::skkserv {

// The two bytes of a cell (row 1-94, column 1-94) as (first << 8) | second.
inline constexpr uint16_t EucCell(int row, int col) {
  return static_cast<uint16_t>(((0xA0 + row) << 8) | (0xA0 + col));
}

class EucJpCodec {
 public:
  EucJpCodec() {
    for (int row = 1; row <= kJisX0208Rows; ++row) {
      for (int col = 1; col <= kJisX0208Columns; ++col) {
        const uint16_t cp = kJisX0208[row - 1][col - 1];
        if (cp != 0) {
          to_euc_.emplace(static_cast<char32_t>(cp), EucCell(row, col));
        }
      }
    }
    // Characters that Windows uses for the same cells.
    struct Alias {
      char32_t cp;
      uint16_t cell;
    };
    static constexpr Alias kAliases[] = {
        {0xFF5E, EucCell(1, 33)}, {0x2225, EucCell(1, 34)},
        {0xFF0D, EucCell(1, 61)}, {0xFFE0, EucCell(1, 81)},
        {0xFFE1, EucCell(1, 82)}, {0xFFE2, EucCell(2, 44)},
    };
    for (const Alias& a : kAliases) {
      to_euc_.emplace(a.cp, a.cell);
    }
  }

  // EUC-JP -> UTF-8. std::nullopt if |euc| is not valid or not supported.
  std::optional<std::string> Decode(const std::string& euc) const {
    std::string out;
    size_t i = 0;
    while (i < euc.size()) {
      const unsigned char b = static_cast<unsigned char>(euc[i]);
      if (b >= 0x20 && b <= 0x7E) {
        out.push_back(static_cast<char>(b));
        ++i;
        continue;
      }
      if (i + 1 >= euc.size()) {
        return std::nullopt;
      }
      const unsigned char t = static_cast<unsigned char>(euc[i + 1]);
      if (b == 0x8E) {  // half-width katakana
        if (t < 0xA1 || t > 0xDF) {
          return std::nullopt;
        }
        out += BmpToUtf8(static_cast<char32_t>(0xFF61 + (t - 0xA1)));
      } else if (b >= 0xA1 && b <= 0xA0 + kJisX0208Rows && t >= 0xA1 &&
                 t <= 0xFE) {
        const uint16_t cp = kJisX0208[b - 0xA1][t - 0xA1];
        if (cp == 0) {
          return std::nullopt;
        }
        out += BmpToUtf8(cp);
      } else {
        return std::nullopt;
      }
      i += 2;
    }
    return out;
  }

  // Characters -> EUC-JP. |chars| is anything that gives char32_t when it is
  // iterated, for example Utf8AsChars32() of Mozc, which decodes UTF-8 one
  // character at a time (a character of an ill-formed sequence is U+FFFD, which
  // cannot be represented). std::nullopt if a character cannot be represented.
  template <typename Chars>
  std::optional<std::string> Encode(const Chars& chars) const {
    std::string out;
    for (const char32_t cp : chars) {
      if (cp >= 0x20 && cp <= 0x7E) {
        out.push_back(static_cast<char>(cp));
      } else if (cp >= 0xFF61 && cp <= 0xFF9F) {
        out.push_back(static_cast<char>(0x8E));
        out.push_back(static_cast<char>(0xA1 + (cp - 0xFF61)));
      } else {
        const auto it = to_euc_.find(cp);
        if (it == to_euc_.end()) {
          return std::nullopt;
        }
        out.push_back(static_cast<char>(it->second >> 8));
        out.push_back(static_cast<char>(it->second & 0xFF));
      }
    }
    return out;
  }

 private:
  // The UTF-8 of a code point below U+10000. Every character that EUC-JP can
  // give (the table and the half-width katakana) is in this range.
  static std::string BmpToUtf8(char32_t cp) {
    if (cp < 0x80) {
      return std::string(1, static_cast<char>(cp));
    }
    if (cp < 0x800) {
      const char bytes[] = {static_cast<char>(0xC0 | (cp >> 6)),
                            static_cast<char>(0x80 | (cp & 0x3F))};
      return std::string(bytes, 2);
    }
    const char bytes[] = {static_cast<char>(0xE0 | (cp >> 12)),
                          static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
                          static_cast<char>(0x80 | (cp & 0x3F))};
    return std::string(bytes, 3);
  }

  std::unordered_map<char32_t, uint16_t> to_euc_;
};

}  // namespace mozc::skkserv

#endif  // MOZC_SKKSERV_EUC_JP_H_
