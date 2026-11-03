#pragma once

// gw2 (#43): Wine's __wine_dbg_output buffers a line per thread in 1020 bytes (ntdll's
// debug_info::output) and raises STATUS_BUFFER_OVERFLOW (0x80000005) for a longer one. The logger
// therefore hands Wine every line in pieces of at most kWineLogPieceMax bytes, each ending in '\n';
// continuations carry the level prefix and kWineLogContinuation. Header-only, so that
// tests/gw2/log_split_test.cpp builds it on the host.

#include <cstddef>
#include <string>

namespace dxmt {

constexpr size_t kWineLogPieceMax = 900;
constexpr const char *kWineLogContinuation = "... ";

// Calls out(piece) for each piece of `prefix + text + '\n'`. Every piece is a std::string
// of at most maxBytes bytes ending in '\n'; the pieces' texts, joined, are `text`. A
// cut never falls inside a UTF-8 sequence unless a sequence is longer than the room for text.
template <typename F>
void
splitLogLine(const std::string &prefix, const std::string &text, F &&out,
             size_t maxBytes = kWineLogPieceMax) {
  const std::string cont = prefix + kWineLogContinuation;
  size_t pos = 0;
  bool first = true;
  do {
    const std::string &head = first ? prefix : cont;
    // room for text: the piece minus its head and the '\n' (at least 1, so the loop ends)
    size_t room = maxBytes > head.size() + 1 ? maxBytes - head.size() - 1 : 1;
    size_t len = text.size() - pos;
    if (len > room) {
      len = room;
      // don't cut a UTF-8 sequence: back up over continuation bytes (10xxxxxx)
      size_t cut = pos + len;
      while (cut > pos && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        cut--;
      if (cut > pos)
        len = cut - pos;
    }
    std::string piece;
    piece.reserve(head.size() + len + 1);
    piece += head;
    piece.append(text, pos, len);
    piece += '\n';
    out(piece);
    pos += len;
    first = false;
  } while (pos < text.size());
}

} // namespace dxmt
