#pragma once

// gw2 (#132): DXMT_DEPENDENCY_COMPRESSION (off by default) and the DEPS statistics section. The
// policy itself is FenceLocalityCheck's (dxmt_deptrack.hpp).

namespace dxmt::gw2 {

// Reads the switch for a queue being made; true when the queue's context uses the compressed
// policy (the switch on and the queue on Metal 4). Logs the outcome once per process.
bool dependencyCompressionAttach(bool metal4);

} // namespace dxmt::gw2
