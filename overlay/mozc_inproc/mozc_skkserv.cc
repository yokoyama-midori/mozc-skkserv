// An SKK dictionary server (the skkserv protocol) that converts with Mozc.
//
// The server keeps Mozc's SessionHandler (the object that mozc_server uses to
// process requests) in its own process, so neither mozc_server nor any helper
// process is needed. It listens on 127.0.0.1 only.
//
// Protocol (the subset used by SKK clients such as CorvusSKK):
//   "1<reading> "  look up   -> "1/<candidate>/.../\n"  or  "4\n" (not found)
//   "2"            version   -> "<version> "
//   "3"            host info -> "<host>/<address>/ "
//   "4<reading> "  complete  -> "4\n" (not supported)
//   "0"            disconnect
// The request and the response use the encoding given by --encoding:
// UTF-8 (default) or EUC-JP. In the EUC-JP mode, a candidate that cannot be
// represented in EUC-JP (for example an emoji) is left out. The conversion uses
// its own table (euc_jp.h), not the code pages of Windows.
//
// A reading is accepted only if it consists of hiragana (U+3041-U+3096),
// U+309B-U+309E and the prolonged sound mark (U+30FC), and is at most 80
// characters long. Anything else is answered with "4\n". For example, an SKK
// client looks up a word with okurigana (such as 送る) by the hiragana stem
// followed by the consonant of the okurigana ("おくr"). Such a reading contains
// a Latin letter and is answered with "4\n".
//
// Derived from src/unix/emacs/mozc_emacs_helper.cc of Mozc: the session is
// created and the keys are sent in the same way.
//
// Copyright (c) 2026 mozc-skkserv contributors (the changes and additions).
// Copyright 2010-2021, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef _WIN32
#error "mozc_skkserv supports Windows only."
#endif  // _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif  // NOMINMAX
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif  // WIN32_LEAN_AND_MEAN
// The order matters: <winsock2.h> must come before <windows.h>.
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/reflection.h"
#include "absl/strings/string_view.h"
#include "base/init_mozc.h"
#include "base/strings/japanese.h"
#include "base/strings/unicode.h"
#include "base/system_util.h"
#include "composer/key_parser.h"
#include "engine/engine_factory.h"
#include "mozc_inproc/euc_jp.h"
#include "protocol/candidate_window.pb.h"
#include "protocol/commands.pb.h"
#include "session/session_handler.h"

#pragma comment(lib, "ws2_32.lib")

ABSL_FLAG(int32_t, port, 1178, "TCP port to listen on (127.0.0.1).");
ABSL_FLAG(std::string, encoding, "utf-8",
          "Encoding of requests and responses: utf-8 or euc-jp.");
ABSL_FLAG(int32_t, max_candidates, 12,
          "Maximum number of candidates in a response.");
ABSL_FLAG(bool, verbose, false,
          "Prints the length of a reading, the number of candidates and the "
          "time to the standard output. The reading itself is never printed.");

namespace mozc::skkserv {
namespace {

// A user profile directory that cannot exist ("<" and ">" are not allowed in a
// Windows path). Mozc is told to use it so that it does not make its default
// profile folder (%USERPROFILE%\AppData\LocalLow\Mozc).
constexpr char kNoProfileDirectory[] = "<no profile>";

constexpr char kVersion[] = "mozc-skkserv/0.1.0";
constexpr size_t kMaxReadingChars = 80;
constexpr int kMaxConnections = 16;
// A valid request is about 250 bytes at most. Anything that grows beyond this
// without a terminating space is not a request.
constexpr size_t kMaxRequestBytes = 4096;

// Called when the server is stopped (Ctrl+C, closing the console window).
// Returning FALSE lets the default handler end the process.
BOOL WINAPI OnConsoleControl(DWORD) {
  std::fprintf(stdout, "%s: stopped\n", kVersion);
  std::fflush(stdout);
  return FALSE;
}

bool IsAcceptedReadingChar(char32_t cp) {
  return (cp >= 0x3041 && cp <= 0x3096) || (cp >= 0x309B && cp <= 0x309E) ||
         cp == 0x30FC;
}

enum class WireEncoding { kUtf8, kEucJp };

bool ContainsReservedChar(const std::string& s) {
  return s.find_first_of("/;\r\n") != std::string::npos;
}

// Builds the candidate list (UTF-8) from the output of the space key.
//  * The first candidate is the joined text of all segments of the first
//    conversion result.
//  * The others are the alternatives of the first segment, followed by the
//    remaining segments.
// Left out are the candidates that Mozc makes only to change the script of the
// reading of the first segment (the reading itself in hiragana, and in
// half-width katakana), a candidate with a character that the protocol uses as
// a separator and the duplicates.
std::vector<std::string> ExtractCandidates(const commands::Output& output,
                                           const std::string& reading) {
  std::vector<std::string> segments;
  for (const auto& segment : output.preedit().segment()) {
    segments.push_back(segment.value());
  }
  if (segments.empty()) {
    return {};
  }
  const commands::Preedit::Segment& first = output.preedit().segment(0);
  const std::string key = first.has_key() ? first.key() : std::string();
  // Kana without a half-width form (such as ゐ) stay full-width in the result
  // and are ordinary candidates, so they are not excluded.
  std::string half_width_key = japanese::HiraganaToHalfwidthKatakana(key);
  if (half_width_key == japanese::HiraganaToKatakana(key)) {
    half_width_key.clear();
  }

  std::string top;
  std::string rest;
  for (size_t i = 0; i < segments.size(); ++i) {
    top += segments[i];
    if (i > 0) {
      rest += segments[i];
    }
  }

  std::vector<std::string> all;
  all.push_back(top);
  for (const auto& candidate : output.all_candidate_words().candidates()) {
    if (!candidate.has_value() || candidate.value() == key ||
        candidate.value() == half_width_key) {
      continue;
    }
    all.push_back(segments.size() == 1 ? candidate.value()
                                       : candidate.value() + rest);
  }

  std::vector<std::string> result;
  for (const auto& c : all) {
    if (c == reading || ContainsReservedChar(c)) {
      continue;
    }
    if (std::find(result.begin(), result.end(), c) != result.end()) {
      continue;
    }
    result.push_back(c);
  }
  return result;
}

// The adapter to Mozc: UTF-8 in, UTF-8 out.
class Converter {
 public:
  explicit Converter(SessionHandler* handler) : handler_(handler) {}

  // Conversions are serialized because SessionHandler is not thread-safe.
  std::vector<std::string> Convert(const std::string& reading) {
    std::lock_guard<std::mutex> lock(mutex_);

    const std::optional<uint64_t> session_id = CreateSession();
    if (!session_id) {
      return {};
    }

    bool ok = true;
    // Utf8AsChars() does not check the encoding. The reading is valid because
    // Answer() accepted only hiragana.
    for (const absl::string_view character : Utf8AsChars(reading)) {
      if (!SendKey(*session_id, {}, character)) {
        ok = false;
        break;
      }
    }
    std::optional<commands::Output> output;
    if (ok) {
      output = SendKey(*session_id, {"space"}, "");
    }

    DeleteSession(*session_id);
    if (!output) {
      return {};
    }
    return ExtractCandidates(*output, reading);
  }

 private:
  std::optional<uint64_t> CreateSession() {
    commands::Command create;
    create.mutable_input()->set_type(commands::Input::CREATE_SESSION);
    if (!handler_->EvalCommand(&create) ||
        create.output().error_code() != commands::Output::SESSION_SUCCESS) {
      return std::nullopt;
    }
    const uint64_t id = create.output().id();

    // On Windows a new session starts in the direct input mode.
    // Switch the IME on and select the hiragana mode.
    commands::Command on;
    commands::Input* input = on.mutable_input();
    input->set_type(commands::Input::SEND_COMMAND);
    input->set_id(id);
    input->mutable_command()->set_type(commands::SessionCommand::TURN_ON_IME);
    input->mutable_command()->set_composition_mode(commands::HIRAGANA);
    if (!handler_->EvalCommand(&on)) {
      DeleteSession(id);
      return std::nullopt;
    }
    return id;
  }

  void DeleteSession(uint64_t id) {
    commands::Command command;
    command.mutable_input()->set_type(commands::Input::DELETE_SESSION);
    command.mutable_input()->set_id(id);
    handler_->EvalCommand(&command);
  }

  std::optional<commands::Output> SendKey(uint64_t id,
                                          const std::vector<std::string>& keys,
                                          absl::string_view key_string) {
    commands::Command command;
    commands::Input* input = command.mutable_input();
    input->set_type(commands::Input::SEND_KEY);
    input->set_id(id);
    if (!KeyParser::ParseKeyVector(keys, input->mutable_key()) &&
        !KeyParser::ParseKey("undefinedkey", input->mutable_key())) {
      return std::nullopt;
    }
    if (!key_string.empty()) {
      input->mutable_key()->set_key_string(key_string);
    }
    if (!handler_->EvalCommand(&command)) {
      return std::nullopt;
    }
    return std::move(*command.mutable_output());
  }

  SessionHandler* handler_;
  std::mutex mutex_;
};

struct ServerContext {
  Converter* converter;
  // Empty if the connection uses UTF-8.
  std::optional<EucJpCodec> euc_jp;
  size_t max_candidates;
  int port;
  bool verbose;
  std::atomic<int> active{0};
};

// Returns the response (in the encoding of the connection) to a look-up
// request. |wire_reading| is the reading as it was received.
std::string Answer(const ServerContext& context,
                   const std::string& wire_reading) {
  static const char kNotFound[] = "4\n";

  const std::optional<std::string> converted =
      context.euc_jp ? context.euc_jp->Decode(wire_reading)
                     : std::optional(wire_reading);
  if (!converted) {
    return kNotFound;
  }
  const std::string& reading = *converted;
  // The length and the hiragana check in one pass. Utf8AsChars32() turns a
  // character of an ill-formed sequence (and a surrogate) into U+FFFD, which is
  // not hiragana, so the reading is refused.
  size_t length = 0;
  for (const char32_t cp : Utf8AsChars32(reading)) {
    if (++length > kMaxReadingChars || !IsAcceptedReadingChar(cp)) {
      return kNotFound;
    }
  }
  if (length == 0) {
    return kNotFound;
  }

  const auto start = std::chrono::steady_clock::now();
  const std::vector<std::string> candidates =
      context.converter->Convert(reading);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();

  // The response is made here, in the encoding of the connection. A candidate
  // that the encoding cannot represent (an emoji in EUC-JP) is left out, and it
  // does not use up the limit.
  std::string response = "1/";
  size_t count = 0;
  std::vector<std::string> sent;
  for (const auto& c : candidates) {
    const std::optional<std::string> wire =
        context.euc_jp ? context.euc_jp->Encode(Utf8AsChars32(c))
                       : std::optional(c);
    if (!wire) {
      continue;
    }
    // Two characters can become the same bytes (in EUC-JP, U+301C and U+FF5E
    // are both "A1 C1"). Send such a candidate once.
    if (std::find(sent.begin(), sent.end(), *wire) != sent.end()) {
      continue;
    }
    sent.push_back(*wire);
    response += *wire;
    response += '/';
    if (++count >= context.max_candidates) {
      break;
    }
  }
  if (context.verbose) {
    std::fprintf(stdout, "query: %zu chars -> %zu candidates (%lld ms)\n",
                 length, count, static_cast<long long>(ms));
    std::fflush(stdout);
  }
  if (count == 0) {
    return kNotFound;
  }
  response += '\n';
  return response;
}

bool SendAll(SOCKET s, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const int n =
        ::send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

void ServeConnection(SOCKET client, ServerContext* context) {
  std::string buffer;
  char chunk[4096];
  bool running = true;
  while (running) {
    const int n = ::recv(client, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      break;
    }
    buffer.append(chunk, static_cast<size_t>(n));

    while (running && !buffer.empty()) {
      const char op = buffer[0];
      if (op == '0') {
        running = false;
      } else if (op == '1' || op == '4') {
        const size_t end = buffer.find(' ', 1);
        if (end == std::string::npos) {
          if (buffer.size() > kMaxRequestBytes) {
            running = false;  // not a request; do not keep growing
          }
          break;
        }
        const std::string reading = buffer.substr(1, end - 1);
        buffer.erase(0, end + 1);
        const std::string response =
            op == '4' ? "4\n" : Answer(*context, reading);
        if (!SendAll(client, response)) {
          running = false;
        }
      } else if (op == '2') {
        buffer.erase(0, 1);
        if (!SendAll(client, std::string(kVersion) + " ")) {
          running = false;
        }
      } else if (op == '3') {
        buffer.erase(0, 1);
        char host[256] = "localhost";
        ::gethostname(host, sizeof(host) - 1);
        if (!SendAll(client, std::string(host) + "/127.0.0.1:" +
                                 std::to_string(context->port) + "/ ")) {
          running = false;
        }
      } else {
        buffer.erase(0, 1);  // ignore unknown bytes such as a trailing newline
      }
    }
  }
  ::closesocket(client);
  --context->active;
}

int RunServer(ServerContext* context) {
  WSADATA wsa;
  if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    std::fprintf(stderr, "WSAStartup failed\n");
    return 1;
  }
  SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    std::fprintf(stderr, "socket failed (%d)\n", ::WSAGetLastError());
    return 1;
  }
  // Another process must not be able to listen on the same port.
  const BOOL exclusive = TRUE;
  ::setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<u_short>(context->port));
  ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (::bind(listener, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0 ||
      ::listen(listener, SOMAXCONN) != 0) {
    const int error = ::WSAGetLastError();
    if (error == WSAEADDRINUSE || error == WSAEACCES) {
      std::fprintf(stderr, "port %d is in use or reserved (%d)\n",
                   context->port, error);
    } else {
      std::fprintf(stderr, "cannot listen on 127.0.0.1:%d (%d)\n",
                   context->port, error);
    }
    return 1;
  }
  std::fprintf(stdout, "%s: listening on 127.0.0.1:%d (%s)\n", kVersion,
               context->port, context->euc_jp ? "euc-jp" : "utf-8");
  std::fflush(stdout);
  ::SetConsoleCtrlHandler(OnConsoleControl, TRUE);

  for (;;) {
    SOCKET client = ::accept(listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) {
      // Do not spin if accept keeps failing.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    if (context->active.load() >= kMaxConnections) {
      ::closesocket(client);
      continue;
    }
    ++context->active;
    std::thread(ServeConnection, client, context).detach();
  }
}

std::optional<WireEncoding> ParseEncoding(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (name == "utf-8" || name == "utf8") {
    return WireEncoding::kUtf8;
  }
  if (name == "euc-jp" || name == "eucjp") {
    return WireEncoding::kEucJp;
  }
  return std::nullopt;
}

// mozc::InitMozc() ignores options that it does not know, so a mistyped option
// (for example --skkserv_encodng=euc-jp) would be ignored without a word.
// This rejects unknown options and stray arguments instead.
bool ValidateArguments(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--") {
      break;
    }
    if (arg.size() < 2 || arg[0] != '-') {
      std::fprintf(stderr, "unexpected argument: %s\n", argv[i]);
      return false;
    }
    const size_t dashes = (arg.size() > 2 && arg[1] == '-') ? 2 : 1;
    const std::string body = arg.substr(dashes);
    const size_t equal = body.find('=');
    const std::string name = body.substr(0, equal);
    const bool has_value = equal != std::string::npos;

    const absl::CommandLineFlag* flag = absl::FindCommandLineFlag(name);
    bool negated = false;  // --nofoo for a bool flag foo
    if (flag == nullptr && name.rfind("no", 0) == 0) {
      flag = absl::FindCommandLineFlag(name.substr(2));
      negated = flag != nullptr;
    }
    if (flag == nullptr || (negated && !flag->IsOfType<bool>())) {
      std::fprintf(stderr, "unknown option: %s\n", argv[i]);
      return false;
    }
    if (!has_value && !negated && !flag->IsOfType<bool>()) {
      // "--name value": the value is in the next argument.
      if (++i >= argc) {
        std::fprintf(stderr, "missing value for: %s\n", arg.c_str());
        return false;
      }
    }
  }
  return true;
}

}  // namespace
}  // namespace mozc::skkserv

int main(int argc, char** argv) {
  // Flags are registered before main(), so unknown options can be found before
  // InitMozc() runs.
  if (!mozc::skkserv::ValidateArguments(argc, argv)) {
    return 1;
  }
  // InitMozc() uses the profile directory to decide the path of the log, so
  // kNoProfileDirectory is set first.
  mozc::SystemUtil::SetUserProfileDirectory(mozc::skkserv::kNoProfileDirectory);
  mozc::InitMozc(argv[0], &argc, &argv);

  const int port = absl::GetFlag(FLAGS_port);
  if (port <= 0 || port > 65535) {
    std::fprintf(stderr, "invalid --port: %d\n", port);
    return 1;
  }
  const int max_candidates = absl::GetFlag(FLAGS_max_candidates);
  if (max_candidates <= 0) {
    std::fprintf(stderr, "invalid --max_candidates\n");
    return 1;
  }
  const std::optional<mozc::skkserv::WireEncoding> encoding =
      mozc::skkserv::ParseEncoding(absl::GetFlag(FLAGS_encoding));
  if (!encoding) {
    std::fprintf(stderr, "invalid --encoding (use utf-8 or euc-jp)\n");
    return 1;
  }

  auto engine = mozc::EngineFactory::Create();
  if (!engine.ok()) {
    std::fprintf(stderr, "cannot create the conversion engine\n");
    return 1;
  }
  mozc::SessionHandler handler(*std::move(engine));
  if (!handler.IsAvailable()) {
    std::fprintf(stderr, "the session handler is not available\n");
    return 1;
  }

  mozc::skkserv::Converter converter(&handler);
  mozc::skkserv::ServerContext context;
  context.converter = &converter;
  if (*encoding == mozc::skkserv::WireEncoding::kEucJp) {
    context.euc_jp.emplace();
  }
  context.max_candidates = static_cast<size_t>(max_candidates);
  context.port = port;
  context.verbose = absl::GetFlag(FLAGS_verbose);
  return mozc::skkserv::RunServer(&context);
}
