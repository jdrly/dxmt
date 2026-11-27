// gw2 (#312): what CommandQueue needs from my fork at its creation, out of dxmt_command_queue.cpp: the
// queue's switches (DXMT_RETIRE_RECOVERY #113, DXMT_RELEASE_POOL #269), the RING section (#8), and
// CommandQueue::gw2AttachSubmission, the constructor's fork part. All of it is defined in
// dxmt_gw2_queue_setup.cpp. A new switch of the queue, or a new step of its bring-up, goes there.
#pragma once

#include "dxmt_command_queue.hpp"
#include <cstdint>
#include <string>

namespace dxmt::gw2 {

// each reads its switch when called, as the constructor's initializers did
bool queueRetireRecovery();
bool queueReleasePool();

// the RING section: CommitCurrentChunk publishes a frame, the 2 s window drains them
void publishRingFrame(const CommandQueue::RingFrame &frame);
std::string ringSection(double window_ms, uint32_t frames);

} // namespace dxmt::gw2
