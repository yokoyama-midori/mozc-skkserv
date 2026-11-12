// Copyright (c) 2026 mozc-skkserv contributors. BSD-3-Clause (see LICENSE).
//
// Checks EucJpCodec against the cases that tests/test_euc_jp.py writes.
// Encode() is called the way the server does: with Utf8AsChars32() of Mozc.
//
// usage: euc_jp_check <cases.tsv>
//
// Each line has tab-separated fields (all bytes in hex):
//   ok        <EUC>    <UTF-8>   Decode and Encode both work
//   encode    <UTF-8>  <EUC>     Encode(UTF-8) gives EUC
//   dec_fail  <EUC>              Decode(EUC) must fail
//   enc_fail  <UTF-8>            Encode(UTF-8) must fail
// The exit code is 0 only if every case passes.
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "base/strings/unicode.h"
#include "mozc_inproc/euc_jp.h"

namespace {

std::string HexToBytes(const std::string& hex) {
  std::string out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

std::vector<std::string> Split(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  std::istringstream in(line);
  while (std::getline(in, field, '\t')) {
    fields.push_back(field);
  }
  return fields;
}

// UTF-8 -> EUC-JP the way the server does it.
std::optional<std::string> Encode(const mozc::skkserv::EucJpCodec& codec,
                                  const std::string& utf8_hex) {
  const std::string utf8 = HexToBytes(utf8_hex);
  return codec.Encode(mozc::Utf8AsChars32(utf8));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: euc_jp_check <cases.tsv>\n");
    return 2;
  }
  std::ifstream file(argv[1]);
  if (!file) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  const mozc::skkserv::EucJpCodec codec;
  std::map<std::string, int> passed;
  int failed = 0;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::vector<std::string> f = Split(line);
    if (f.empty()) {
      continue;
    }
    bool good = false;
    // Comparing a std::optional with a value is true only if it has the value.
    if (f[0] == "ok" && f.size() == 3) {
      good = codec.Decode(HexToBytes(f[1])) == HexToBytes(f[2]) &&
             Encode(codec, f[2]) == HexToBytes(f[1]);
    } else if (f[0] == "encode" && f.size() == 3) {
      good = Encode(codec, f[1]) == HexToBytes(f[2]);
    } else if (f[0] == "dec_fail" && f.size() == 2) {
      good = !codec.Decode(HexToBytes(f[1])).has_value();
    } else if (f[0] == "enc_fail" && f.size() == 2) {
      good = !Encode(codec, f[1]).has_value();
    } else {
      std::printf("bad line: %s\n", line.c_str());
    }
    if (good) {
      ++passed[f[0]];
    } else {
      ++failed;
      if (failed <= 20) {
        std::printf("FAILED: %s\n", line.c_str());
      }
    }
  }
  for (const auto& [kind, count] : passed) {
    std::printf("%-9s passed %d\n", kind.c_str(), count);
  }
  std::printf("failed %d\n", failed);
  return failed == 0 ? 0 : 1;
}
