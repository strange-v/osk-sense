#pragma once

namespace radiosensors::node::stack_monitor {
#if defined(NODE_STACK_DIAGNOSTICS)
#if !defined(NODE_DEBUG)
#error "NODE_STACK_DIAGNOSTICS requires NODE_DEBUG serial output"
#endif
void report();
struct Scope { ~Scope() { report(); } };
#else
struct Scope {};
#endif
}  // namespace radiosensors::node::stack_monitor
