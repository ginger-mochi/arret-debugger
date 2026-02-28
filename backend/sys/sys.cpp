/*
 * sys.cpp: System descriptor dispatcher
 */

#include "sys.hpp"
#include <string.h>

namespace sys {

// Forward declarations for per-system Sys instances
extern const Sys sys_gb;
extern const Sys sys_gbc;
extern const Sys sys_nes;
extern const Sys sys_psx;

static const Sys *sys_table[] = {
    &sys_gb,
    &sys_gbc,
    &sys_nes,
    &sys_psx,
};

const Sys *sys_for_desc(const char *description)
{
    if (!description) return nullptr;
    for (auto *s : sys_table)
        if (strcmp(s->description, description) == 0)
            return s;
    return nullptr;
}

} // namespace sys
