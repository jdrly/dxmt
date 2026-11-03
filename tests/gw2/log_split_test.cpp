// gw2 (#43): host-side test of the logger's split for Wine's debug output (splitLogLine,
// forEachLogLine, writeWineLine). The fake below copies ntdll's __wine_dbg_output and append_output
// (dlls/ntdll/thread.c, CX 26.3): a 1020-byte per-thread buffer that raises STATUS_BUFFER_OVERFLOW
// for a longer line. Not part of the DLL build; run it on the Mac:
//
//   clang++ -std=c++20 -Isrc/util/log tests/gw2/log_split_test.cpp -o /tmp/log_split_test \
//     && /tmp/log_split_test

#include "log_split.hpp"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

using namespace dxmt;

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

namespace fake_wine {
char output[1020];
size_t out_pos = 0;
bool overflow = false;
std::string written; // what reached stderr

bool
append_output(const char *str, size_t len) {
  if (len >= sizeof(output) - out_pos) {
    overflow = true; // Wine: RtlRaiseStatus(STATUS_BUFFER_OVERFLOW)
    out_pos = 0;
    return false;
  }
  memcpy(output + out_pos, str, len);
  out_pos += len;
  return true;
}

int
dbg_output(const char *str) {
  const char *end = strrchr(str, '\n');
  if (end) {
    if (!append_output(str, end + 1 - str))
      return 0;
    written.append(output, out_pos);
    out_pos = 0;
    str = end + 1;
  }
  if (*str)
    append_output(str, strlen(str));
  return 0;
}

void
reset() {
  out_pos = 0;
  overflow = false;
  written.clear();
}
} // namespace fake_wine

static const std::string prefix = "info:  ";

// Splits `text`, sends it through the fake, and checks every piece and the reassembled text.
static std::vector<std::string>
run(const std::string &text, size_t maxBytes = kWineLogPieceMax) {
  fake_wine::reset();
  std::vector<std::string> pieces;
  splitLogLine(
      prefix, text,
      [&](const std::string &piece) {
        pieces.push_back(piece);
        fake_wine::dbg_output(piece.c_str());
      },
      maxBytes
  );
  CHECK(!fake_wine::overflow);
  CHECK(fake_wine::out_pos == 0); // every piece ended its line
  std::string joined;
  for (size_t i = 0; i < pieces.size(); i++) {
    const std::string &p = pieces[i];
    CHECK(p.size() <= maxBytes);
    CHECK(!p.empty() && p.back() == '\n');
    CHECK(p.find('\n') == p.size() - 1);
    std::string head = i == 0 ? prefix : prefix + kWineLogContinuation;
    CHECK(p.compare(0, head.size(), head) == 0);
    joined += p.substr(head.size(), p.size() - head.size() - 1);
  }
  CHECK(joined == text);
  CHECK(fake_wine::written.size() == [&] {
    size_t n = 0;
    for (auto &p : pieces)
      n += p.size();
    return n;
  }());
  return pieces;
}

int
main() {
  // the fake reproduces the crash: a whole 1,100-byte line overflows Wine's buffer
  fake_wine::reset();
  std::string whole = prefix + std::string(1100, 'x') + "\n";
  fake_wine::dbg_output(whole.c_str());
  CHECK(fake_wine::overflow);

  // short lines stay one piece, unchanged
  CHECK(run("") == std::vector<std::string>{prefix + "\n"});
  CHECK(run("hello") == std::vector<std::string>{prefix + "hello\n"});

  // the issue's case: 5,000 characters
  std::string big;
  for (int i = 0; big.size() < 5000; i++)
    big += std::to_string(i) + " ";
  big.resize(5000);
  CHECK(run(big).size() == 6);

  // exactly at the boundary: 900 - 7 (prefix) - 1 ('\n') = 892 bytes fit in one piece
  CHECK(run(std::string(892, 'a')).size() == 1);
  CHECK(run(std::string(893, 'a')).size() == 2);

  // UTF-8: a cut backs up rather than splitting a sequence ("é" is 2 bytes, "€" 3)
  for (size_t shift = 0; shift < 4; shift++) {
    std::string utf = std::string(shift, 'a');
    while (utf.size() < 3000)
      utf += "é€";
    auto pieces = run(utf);
    for (size_t i = 1; i < pieces.size(); i++) {
      size_t head = prefix.size() + strlen(kWineLogContinuation);
      CHECK((static_cast<unsigned char>(pieces[i][head]) & 0xC0) != 0x80);
    }
  }

  // a tiny limit still makes progress and never exceeds it
  run(std::string(50, 'b'), 16);

  // writeWineLine, as Logger::emitMsg calls it, through the fake with its buffer kept between calls
  fake_wine::reset();
  auto wine = [&](const std::string &line, std::vector<std::string> *pieces = nullptr) {
    std::string adjusted = prefix + line + "\n";
    bool hasNul = line.find('\0') != std::string::npos;
    writeWineLine(prefix.c_str(), line, adjusted, hasNul, [&](const std::string &piece) {
      if (pieces)
        pieces->push_back(piece);
      fake_wine::dbg_output(piece.c_str());
    });
  };

  // a short line is passed as is (the same string, no copy)
  {
    std::string line = "hello", adjusted = prefix + line + "\n";
    const std::string *seen = nullptr;
    writeWineLine(prefix.c_str(), line, adjusted, false, [&](const std::string &piece) {
      seen = &piece;
    });
    CHECK(seen == &adjusted);
  }

  // the review's case: a NUL would hide the first piece's '\n', leave 207 bytes pending in Wine's
  // buffer and overflow it with the next piece. NULs are written as "\0" instead.
  {
    std::string line = std::string(200, 'x') + std::string(1, '\0') + std::string(1600, 'y');
    fake_wine::reset(); // the fake reproduces it: unescaped pieces overflow
    splitLogLine(prefix, line, [](const std::string &piece) { fake_wine::dbg_output(piece.c_str()); });
    CHECK(fake_wine::overflow);
    fake_wine::reset();
    std::vector<std::string> pieces;
    wine(line, &pieces);
    CHECK(!fake_wine::overflow);
    CHECK(fake_wine::out_pos == 0);
    CHECK(pieces.size() == 3);
    CHECK(fake_wine::written.find(std::string(200, 'x') + "\\0y") != std::string::npos);
    wine(line); // again, with whatever the first call left
    CHECK(!fake_wine::overflow);
    CHECK(fake_wine::out_pos == 0);
  }

  // a short line with a NUL is escaped too, and a long one without goes in pieces
  wine(std::string("a") + '\0' + "b");
  CHECK(fake_wine::written.find(prefix + "a\\0b\n") != std::string::npos);
  wine(std::string(5000, 'z'));
  CHECK(!fake_wine::overflow);
  CHECK(fake_wine::out_pos == 0);

  // forEachLogLine gives the lines std::getline gave, and finds the NULs in them
  for (const std::string &m : {std::string(""), std::string("a"), std::string("a\n"),
                               std::string("\n"), std::string("\n\n"), std::string("a\n\nb"),
                               std::string("a\nb\n"), std::string("x\0y\nz", 5),
                               std::string("\0\n\0", 3)}) {
    std::vector<std::string> expect, got;
    std::vector<bool> nuls;
    std::stringstream stream(m);
    std::string line;
    while (std::getline(stream, line, '\n'))
      expect.push_back(line);
    forEachLogLine(m, [&](const std::string &l, bool hasNul) {
      got.push_back(l);
      nuls.push_back(hasNul);
    });
    CHECK(got == expect);
    for (size_t i = 0; i < got.size(); i++)
      CHECK(nuls[i] == (got[i].find('\0') != std::string::npos));
  }

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("log_split_test: ok\n");
  return 0;
}
