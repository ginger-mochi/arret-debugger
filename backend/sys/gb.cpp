/*
 * gb.cpp: Game Boy / Game Boy Color system descriptors
 */

#include "sys.hpp"

namespace sys {

static const char *const gb_int_names[] = {
    "VBlank",
    "STAT",
    "Timer",
    "Serial",
    "Joypad",
};

extern const Sys sys_gb  = { "gb",  gb_int_names, 5 };
extern const Sys sys_gbc = { "gbc", gb_int_names, 5 };

} // namespace sys
