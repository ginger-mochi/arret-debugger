/*
 * nes.cpp: NES system descriptor
 */

#include "sys.hpp"

namespace sys {

static const char *const nes_int_names[] = {
    "NMI",
    "IRQ",
};

extern const Sys sys_nes = { "nes", nes_int_names, 2 };

} // namespace sys
