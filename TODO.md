# TODO

## Thread safety: shared booleans without atomics

Several shared variables between the core thread and main/TCP threads use plain
`bool` without `std::atomic`. Examples: `content_loaded`, `frame_ready`,
`bp_hit`. While x86 makes this mostly benign, it's technically UB under the C++
memory model and could break on weaker architectures. Converting these to
`std::atomic<bool>` or adding proper synchronization would be correct.

## Breakpoint conditions: stored but never evaluated

`ar_breakpoint` has a `condition` field (string) that is parsed and stored via
`bp add <addr> [condition]`, but no evaluation engine exists — the condition is
ignored when deciding whether to actually break. Implementing a simple expression
evaluator (register values, memory reads, comparisons) would make conditional
breakpoints functional.
