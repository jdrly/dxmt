#pragma once

// gw2 (#7): the control file, DXMT_CONTROL=<path>, for A/B runs within one start of the game.
//
// A thread of its own reads the file once a second (never the render thread) and publishes each key
// it knows as an atomic; the code that owns the setting reads that atomic where it already uses the
// setting and logs when the value it uses changes. The file is the whole state: a key that is
// missing, a value that isn't understood, or a file that can't be read leaves that setting at its
// environment value. One `key=value` per line; blank lines and lines starting with `#` are skipped.
// An unknown key is logged once and ignored. Unset (the default), no thread and no file access.
//
// Keys:
// - present_deadline=refresh|<ms>|0|default: as DXMT_PRESENT_DEADLINE (one refresh, that many ms
//   (0 < ms <= 1000), off); default = DXMT_PRESENT_DEADLINE's own value.

#include <climits>
#include <cstdint>
#include <string>

namespace dxmt::gw2 {

// DXMT_CONTROL as set ("" when unset: the control file is off).
const std::string &controlPath();

// Starts the poll thread once per process, if DXMT_CONTROL is set. Any thread, any number of times.
void controlStart();

// present_deadline from the file, in µs: kControlUnset (no override: DXMT_PRESENT_DEADLINE),
// -1 one refresh, 0 off, > 0 that many µs. One relaxed atomic load.
constexpr int32_t kControlUnset = INT32_MIN;
int32_t controlPresentDeadlineUs();

} // namespace dxmt::gw2
