/*
 * psx.cpp: PlayStation system descriptor
 *
 * Includes BIOS call tracing: subscribes to execution at 00A0, 00B0, 00C0,
 * reads R9 for function number, reads R4-R7 for arguments, and logs a
 * formatted BIOS call line.
 */

#include "sys.hpp"
#include "retrodebug_psx.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

namespace sys {

/* ========================================================================
 * Interrupt names
 * ======================================================================== */

static const char *const psx_int_names[] = {
    "VBlank",
    "GPU",
    "CD",
    "DMA",
    "Timer0",
    "Timer1",
    "Timer2",
    "SIO",
    nullptr,    // bit 8: no standard interrupt
    "SPU",
    "PIO",
};

/* ========================================================================
 * BIOS function tables
 * ======================================================================== */

struct BiosFunc {
    uint8_t func;         // R9 value
    const char *name;
    uint8_t nargs;        // number of named arguments (0-4)
};

/* A-functions (call 00A0h) */
static const BiosFunc a_table[] = {
    { 0x00, "FileOpen", 2 },
    { 0x01, "FileSeek", 3 },
    { 0x02, "FileRead", 3 },
    { 0x03, "FileWrite", 3 },
    { 0x04, "FileClose", 1 },
    { 0x05, "FileIoctl", 3 },
    { 0x06, "exit", 1 },
    { 0x07, "FileGetDeviceFlag", 1 },
    { 0x08, "FileGetc", 1 },
    { 0x09, "FilePutc", 2 },
    { 0x0A, "todigit", 1 },
    { 0x0B, "atof", 1 },
    { 0x0C, "strtoul", 3 },
    { 0x0D, "strtol", 3 },
    { 0x0E, "abs", 1 },
    { 0x0F, "labs", 1 },
    { 0x10, "atoi", 1 },
    { 0x11, "atol", 1 },
    { 0x12, "atob", 2 },
    { 0x13, "SaveState", 1 },
    { 0x14, "RestoreState", 2 },
    { 0x15, "strcat", 2 },
    { 0x16, "strncat", 3 },
    { 0x17, "strcmp", 2 },
    { 0x18, "strncmp", 3 },
    { 0x19, "strcpy", 2 },
    { 0x1A, "strncpy", 3 },
    { 0x1B, "strlen", 1 },
    { 0x1C, "index", 2 },
    { 0x1D, "rindex", 2 },
    { 0x1E, "strchr", 2 },
    { 0x1F, "strrchr", 2 },
    { 0x20, "strpbrk", 2 },
    { 0x21, "strspn", 2 },
    { 0x22, "strcspn", 2 },
    { 0x23, "strtok", 2 },
    { 0x24, "strstr", 2 },
    { 0x25, "toupper", 1 },
    { 0x26, "tolower", 1 },
    { 0x27, "bcopy", 3 },
    { 0x28, "bzero", 2 },
    { 0x29, "bcmp", 3 },
    { 0x2A, "memcpy", 3 },
    { 0x2B, "memset", 3 },
    { 0x2C, "memmove", 3 },
    { 0x2D, "memcmp", 3 },
    { 0x2E, "memchr", 3 },
    { 0x2F, "rand", 0 },
    { 0x30, "srand", 1 },
    { 0x31, "qsort", 4 },
    { 0x32, "strtod", 2 },
    { 0x33, "malloc", 1 },
    { 0x34, "free", 1 },
    { 0x35, "lsearch", 4 },
    { 0x36, "bsearch", 4 },
    { 0x37, "calloc", 2 },
    { 0x38, "realloc", 2 },
    { 0x39, "InitHeap", 2 },
    { 0x3A, "SystemErrorExit", 1 },
    { 0x3B, "std_in_getchar", 0 },
    { 0x3C, "std_out_putchar", 1 },
    { 0x3D, "std_in_gets", 1 },
    { 0x3E, "std_out_puts", 1 },
    { 0x3F, "printf", 1 },  /* variadic -- show r4 + ... */
    { 0x40, "SystemErrorUnresolvedException", 0 },
    { 0x41, "LoadExeHeader", 2 },
    { 0x42, "LoadExeFile", 2 },
    { 0x43, "DoExecute", 3 },
    { 0x44, "FlushCache", 0 },
    { 0x45, "init_a0_b0_c0_vectors", 0 },
    { 0x46, "GPU_dw", 4 },
    { 0x47, "gpu_send_dma", 4 },
    { 0x48, "SendGP1Command", 1 },
    { 0x49, "GPU_cw", 1 },
    { 0x4A, "GPU_cwp", 2 },
    { 0x4B, "send_gpu_linked_list", 1 },
    { 0x4C, "gpu_abort_dma", 0 },
    { 0x4D, "GetGPUStatus", 0 },
    { 0x4E, "gpu_sync", 0 },
    { 0x51, "LoadAndExecute", 3 },
    { 0x54, "CdInit", 0 },
    { 0x55, "_bu_init", 0 },
    { 0x56, "CdRemove", 0 },
    { 0x5B, "dev_tty_init", 0 },
    { 0x5C, "dev_tty_open", 3 },
    { 0x5D, "dev_tty_in_out", 2 },
    { 0x5E, "dev_tty_ioctl", 3 },
    { 0x5F, "dev_cd_open", 3 },
    { 0x60, "dev_cd_read", 3 },
    { 0x61, "dev_cd_close", 1 },
    { 0x62, "dev_cd_firstfile", 3 },
    { 0x63, "dev_cd_nextfile", 2 },
    { 0x64, "dev_cd_chdir", 2 },
    { 0x65, "dev_card_open", 3 },
    { 0x66, "dev_card_read", 3 },
    { 0x67, "dev_card_write", 3 },
    { 0x68, "dev_card_close", 1 },
    { 0x69, "dev_card_firstfile", 3 },
    { 0x6A, "dev_card_nextfile", 2 },
    { 0x6B, "dev_card_erase", 2 },
    { 0x6C, "dev_card_undelete", 2 },
    { 0x6D, "dev_card_format", 1 },
    { 0x6E, "dev_card_rename", 4 },
    { 0x70, "_bu_init", 0 },
    { 0x71, "CdInit", 0 },
    { 0x72, "CdRemove", 0 },
    { 0x78, "CdAsyncSeekL", 1 },
    { 0x7C, "CdAsyncGetStatus", 1 },
    { 0x7E, "CdAsyncReadSector", 3 },
    { 0x81, "CdAsyncSetMode", 1 },
    { 0x90, "CdromIoIrqFunc1", 0 },
    { 0x91, "CdromDmaIrqFunc1", 0 },
    { 0x92, "CdromIoIrqFunc2", 0 },
    { 0x93, "CdromDmaIrqFunc2", 0 },
    { 0x94, "CdromGetInt5errCode", 2 },
    { 0x95, "CdInitSubFunc", 0 },
    { 0x96, "AddCDROMDevice", 0 },
    { 0x97, "AddMemCardDevice", 0 },
    { 0x98, "AddDuartTtyDevice", 0 },
    { 0x99, "AddDummyTtyDevice", 0 },
    { 0x9C, "SetConf", 3 },
    { 0x9D, "GetConf", 3 },
    { 0x9E, "SetCdromIrqAutoAbort", 2 },
    { 0x9F, "SetMemSize", 1 },
    { 0xA0, "WarmBoot", 0 },
    { 0xA1, "SystemErrorBootOrDiskFailure", 2 },
    { 0xA2, "EnqueueCdIntr", 0 },
    { 0xA3, "DequeueCdIntr", 0 },
    { 0xA4, "CdGetLbn", 1 },
    { 0xA5, "CdReadSector", 3 },
    { 0xA6, "CdGetStatus", 0 },
    { 0xAB, "_card_info", 1 },
    { 0xAC, "_card_async_load_directory", 1 },
    { 0xAD, "set_card_auto_format", 1 },
    { 0xAF, "card_write_test", 1 },
    { 0xB2, "ioabort_raw", 1 },
    { 0xB4, "GetSystemInfo", 1 },
};

/* B-functions (call 00B0h) */
static const BiosFunc b_table[] = {
    { 0x00, "alloc_kernel_memory", 1 },
    { 0x01, "free_kernel_memory", 1 },
    { 0x02, "init_timer", 3 },
    { 0x03, "get_timer", 1 },
    { 0x04, "enable_timer_irq", 1 },
    { 0x05, "disable_timer_irq", 1 },
    { 0x06, "restart_timer", 1 },
    { 0x07, "DeliverEvent", 2 },
    { 0x08, "OpenEvent", 4 },
    { 0x09, "CloseEvent", 1 },
    { 0x0A, "WaitEvent", 1 },
    { 0x0B, "TestEvent", 1 },
    { 0x0C, "EnableEvent", 1 },
    { 0x0D, "DisableEvent", 1 },
    { 0x0E, "OpenThread", 3 },
    { 0x0F, "CloseThread", 1 },
    { 0x10, "ChangeThread", 1 },
    { 0x12, "InitPad", 4 },
    { 0x13, "StartPad", 0 },
    { 0x14, "StopPad", 0 },
    { 0x15, "OutdatedPadInitAndStart", 4 },
    { 0x16, "OutdatedPadGetButtons", 0 },
    { 0x17, "ReturnFromException", 0 },
    { 0x18, "SetDefaultExitFromException", 0 },
    { 0x19, "SetCustomExitFromException", 1 },
    { 0x20, "UnDeliverEvent", 2 },
    { 0x32, "FileOpen", 2 },
    { 0x33, "FileSeek", 3 },
    { 0x34, "FileRead", 3 },
    { 0x35, "FileWrite", 3 },
    { 0x36, "FileClose", 1 },
    { 0x37, "FileIoctl", 3 },
    { 0x38, "exit", 1 },
    { 0x39, "FileGetDeviceFlag", 1 },
    { 0x3A, "FileGetc", 1 },
    { 0x3B, "FilePutc", 2 },
    { 0x3C, "std_in_getchar", 0 },
    { 0x3D, "std_out_putchar", 1 },
    { 0x3E, "std_in_gets", 1 },
    { 0x3F, "std_out_puts", 1 },
    { 0x40, "chdir", 1 },
    { 0x41, "FormatDevice", 1 },
    { 0x42, "firstfile", 2 },
    { 0x43, "nextfile", 1 },
    { 0x44, "FileRename", 2 },
    { 0x45, "FileDelete", 1 },
    { 0x46, "FileUndelete", 1 },
    { 0x47, "AddDevice", 1 },
    { 0x48, "RemoveDevice", 1 },
    { 0x49, "PrintInstalledDevices", 0 },
    { 0x4A, "InitCard", 1 },
    { 0x4B, "StartCard", 0 },
    { 0x4C, "StopCard", 0 },
    { 0x4D, "_card_info_subfunc", 1 },
    { 0x4E, "write_card_sector", 3 },
    { 0x4F, "read_card_sector", 3 },
    { 0x50, "allow_new_card", 0 },
    { 0x51, "Krom2RawAdd", 1 },
    { 0x53, "Krom2Offset", 1 },
    { 0x54, "GetLastError", 0 },
    { 0x55, "GetLastFileError", 1 },
    { 0x56, "GetC0Table", 0 },
    { 0x57, "GetB0Table", 0 },
    { 0x58, "get_bu_callback_port", 0 },
    { 0x59, "testdevice", 1 },
    { 0x5B, "ChangeClearPad", 1 },
    { 0x5C, "get_card_status", 1 },
    { 0x5D, "wait_card_status", 1 },
};

/* C-functions (call 00C0h) */
static const BiosFunc c_table[] = {
    { 0x00, "EnqueueTimerAndVblankIrqs", 1 },
    { 0x01, "EnqueueSyscallHandler", 1 },
    { 0x02, "SysEnqIntRP", 2 },
    { 0x03, "SysDeqIntRP", 2 },
    { 0x04, "get_free_EvCB_slot", 0 },
    { 0x05, "get_free_TCB_slot", 0 },
    { 0x06, "ExceptionHandler", 0 },
    { 0x07, "InstallExceptionHandlers", 0 },
    { 0x08, "SysInitMemory", 2 },
    { 0x09, "SysInitKernelVariables", 0 },
    { 0x0A, "ChangeClearRCnt", 2 },
    { 0x0C, "InitDefInt", 1 },
    { 0x0D, "SetIrqAutoAck", 2 },
    { 0x12, "InstallDevices", 1 },
    { 0x13, "FlushStdInOutPut", 0 },
    { 0x15, "tty_cdevinput", 2 },
    { 0x16, "tty_cdevscan", 0 },
    { 0x17, "tty_circgetc", 1 },
    { 0x18, "tty_circputc", 2 },
    { 0x19, "ioabort", 2 },
    { 0x1A, "set_card_find_mode", 1 },
    { 0x1B, "KernelRedirect", 1 },
    { 0x1C, "AdjustA0Table", 0 },
    { 0x1D, "get_card_find_mode", 0 },
};

/* ========================================================================
 * BIOS call trace option
 * ======================================================================== */

/* Register indices (from retrodebug.h RD_R3000A_*) */
#define REG_R4  4
#define REG_R5  5
#define REG_R6  6
#define REG_R7  7
#define REG_R9  9   /* RD_R3000A_T1 = register index 9 */

static rd_SubscriptionID g_bios_subs[3] = { -1, -1, -1 };
static trace_log_fn g_bios_log_fn = nullptr;
static rd_Cpu const *g_bios_cpu = nullptr;

static const BiosFunc *lookup(const BiosFunc *table, unsigned count, uint8_t func) {
    for (unsigned i = 0; i < count; i++)
        if (table[i].func == func) return &table[i];
    return nullptr;
}

static void format_bios_call(char *out, size_t out_size,
                              char table_letter, uint8_t func,
                              rd_Cpu const *cpu)
{
    const BiosFunc *tables[] = { a_table, b_table, c_table };
    const unsigned counts[] = {
        (unsigned)(sizeof(a_table) / sizeof(a_table[0])),
        (unsigned)(sizeof(b_table) / sizeof(b_table[0])),
        (unsigned)(sizeof(c_table) / sizeof(c_table[0])),
    };
    int ti = (table_letter == 'A') ? 0 : (table_letter == 'B') ? 1 : 2;

    const BiosFunc *bf = lookup(tables[ti], counts[ti], func);

    /* Read argument registers */
    uint32_t r4 = (uint32_t)cpu->v1.get_register(cpu, REG_R4);
    uint32_t r5 = (uint32_t)cpu->v1.get_register(cpu, REG_R5);
    uint32_t r6 = (uint32_t)cpu->v1.get_register(cpu, REG_R6);
    uint32_t r7 = (uint32_t)cpu->v1.get_register(cpu, REG_R7);
    uint32_t args[4] = { r4, r5, r6, r7 };

    int pos = 0;
    if (bf) {
        pos += snprintf(out + pos, out_size - pos, "%c%02X: %s(",
                        table_letter, func, bf->name);
        for (uint8_t i = 0; i < bf->nargs && i < 4; i++) {
            if (i > 0)
                pos += snprintf(out + pos, out_size - pos, ", ");
            pos += snprintf(out + pos, out_size - pos, "%X", args[i]);
        }
        /* printf is variadic: show ... after first arg */
        if (bf->func == 0x3F && table_letter == 'A')
            pos += snprintf(out + pos, out_size - pos, ", ...");
        pos += snprintf(out + pos, out_size - pos, ")");
    } else {
        /* Unknown function — show all 4 args */
        pos += snprintf(out + pos, out_size - pos,
                        "%c%02X(%X, %X, %X, %X)",
                        table_letter, func, r4, r5, r6, r7);
    }

    if (pos >= (int)out_size) pos = (int)out_size - 1;
    out[pos] = '\0';
}

static bool psx_trace_on_event(rd_SubscriptionID sub_id,
                               rd_Event const *event)
{
    if (event->type != RD_EVENT_EXECUTION) return false;
    if (!g_bios_log_fn || !g_bios_cpu) return false;

    /* Determine which table based on address */
    uint64_t addr = event->execution.address;
    char table_letter;
    if (addr == 0xA0)      table_letter = 'A';
    else if (addr == 0xB0) table_letter = 'B';
    else if (addr == 0xC0) table_letter = 'C';
    else return false;

    /* Read R9 (T1) for function number */
    uint8_t func = (uint8_t)g_bios_cpu->v1.get_register(g_bios_cpu, REG_R9);

    char line[256];
    format_bios_call(line, sizeof(line), table_letter, func, g_bios_cpu);
    g_bios_log_fn(line);

    (void)sub_id;
    return false;
}

/* ========================================================================
 * GPU command trace option
 * ======================================================================== */

static rd_SubscriptionID g_gpu_subs[2] = { -1, -1 };  // [0]=GP0, [1]=GP1
static trace_log_fn g_gpu_log_fn = nullptr;

static rd_SubscriptionID g_gpu_post_sub = -1;
static trace_log_fn g_gpu_post_log_fn = nullptr;

/* Sign-extend an 11-bit value */
static inline int sign11(uint32_t v) {
    return (int)(v << 21) >> 21;
}

void decode_gp0(char *out, size_t out_size,
                const uint32_t *words, unsigned count)
{
    uint8_t op = (uint8_t)(words[0] >> 24);
    int pos = 0;

    if (op == 0x00) {
        pos = snprintf(out, out_size, "GP0 NOP");
    } else if (op == 0x01) {
        pos = snprintf(out, out_size, "GP0 ClearCache");
    } else if (op == 0x02) {
        /* FillRect: word[1]=(Y<<16|X), word[2]=(H<<16|W) */
        uint32_t c = words[0] & 0xFFFFFF;
        int x = 0, y = 0, w = 0, h = 0;
        if (count >= 3) {
            x = sign11(words[1] & 0x7FF);
            y = sign11((words[1] >> 16) & 0x7FF);
            w = words[2] & 0xFFFF;
            h = words[2] >> 16;
        }
        pos = snprintf(out, out_size, "GP0 FillRect (%d,%d) %dx%d #%06X",
                        x, y, w, h, c);
    } else if (op == 0x1F) {
        pos = snprintf(out, out_size, "GP0 IRQ");
    } else if (op >= 0x20 && op <= 0x3F) {
        /* Polygon */
        bool quad    = op & 0x08;
        bool tex     = op & 0x04;
        bool shade   = op & 0x10;
        bool trans   = op & 0x02;
        pos = snprintf(out, out_size, "GP0 %s %s%s%s",
                        quad ? "Poly4" : "Poly3",
                        shade ? "shade " : "mono ",
                        tex ? "tex " : "",
                        trans ? "trans" : "opaque");
    } else if (op >= 0x40 && op <= 0x5F) {
        /* Line */
        bool shade = op & 0x10;
        bool trans = op & 0x02;
        bool pline = op & 0x08;
        pos = snprintf(out, out_size, "GP0 %s %s%s",
                        pline ? "Polyline" : "Line",
                        shade ? "shade " : "mono ",
                        trans ? "trans" : "opaque");
    } else if (op >= 0x60 && op <= 0x7F) {
        /* Rectangle */
        bool tex   = op & 0x04;
        bool trans = op & 0x02;
        unsigned sz = (op >> 3) & 0x03;
        const char *szname;
        switch (sz) {
        case 0: szname = "var"; break;
        case 1: szname = "1x1"; break;
        case 2: szname = "8x8"; break;
        case 3: szname = "16x16"; break;
        default: szname = "?"; break;
        }
        pos = snprintf(out, out_size, "GP0 Rect %s%s%s",
                        szname,
                        tex ? " tex" : "",
                        trans ? " trans" : " opaque");
    } else if (op >= 0x80 && op <= 0x9F) {
        /* VRAM-to-VRAM copy */
        int sx = 0, sy = 0, dx = 0, dy = 0, w = 0, h = 0;
        if (count >= 4) {
            sx = words[1] & 0x3FF; sy = (words[1] >> 16) & 0x3FF;
            dx = words[2] & 0x3FF; dy = (words[2] >> 16) & 0x3FF;
            w  = words[3] & 0x3FF; h  = (words[3] >> 16) & 0x1FF;
        }
        pos = snprintf(out, out_size, "GP0 VRAM>VRAM (%d,%d)>(%d,%d) %dx%d",
                        sx, sy, dx, dy, w, h);
    } else if (op >= 0xA0 && op <= 0xBF) {
        /* CPU-to-VRAM */
        int x = 0, y = 0, w = 0, h = 0;
        if (count >= 3) {
            x = words[1] & 0x3FF; y = (words[1] >> 16) & 0x3FF;
            w = words[2] & 0x3FF; h = (words[2] >> 16) & 0x1FF;
        }
        pos = snprintf(out, out_size, "GP0 CPU>VRAM (%d,%d) %dx%d",
                        x, y, w, h);
    } else if (op >= 0xC0 && op <= 0xDF) {
        /* VRAM-to-CPU */
        int x = 0, y = 0, w = 0, h = 0;
        if (count >= 3) {
            x = words[1] & 0x3FF; y = (words[1] >> 16) & 0x3FF;
            w = words[2] & 0x3FF; h = (words[2] >> 16) & 0x1FF;
        }
        pos = snprintf(out, out_size, "GP0 VRAM>CPU (%d,%d) %dx%d",
                        x, y, w, h);
    } else if (op == 0xE1) {
        uint32_t v = words[0] & 0xFFFFFF;
        unsigned texpage_x = (v & 0xF) * 64;
        unsigned texpage_y = ((v >> 4) & 1) * 256;
        unsigned abr = (v >> 5) & 3;
        unsigned tp = (v >> 7) & 3;
        const char *depth[] = { "4bpp", "8bpp", "15bpp", "reserved" };
        bool dither = (v >> 9) & 1;
        pos = snprintf(out, out_size, "GP0 DrawMode page=(%u,%u) abr=%u %s%s",
                        texpage_x, texpage_y, abr, depth[tp],
                        dither ? " dither" : "");
    } else if (op == 0xE2) {
        pos = snprintf(out, out_size, "GP0 TexWindow %08X", words[0] & 0xFFFFFF);
    } else if (op == 0xE3) {
        unsigned x = words[0] & 0x3FF;
        unsigned y = (words[0] >> 10) & 0x1FF;
        pos = snprintf(out, out_size, "GP0 DrawAreaTL (%u,%u)", x, y);
    } else if (op == 0xE4) {
        unsigned x = words[0] & 0x3FF;
        unsigned y = (words[0] >> 10) & 0x1FF;
        pos = snprintf(out, out_size, "GP0 DrawAreaBR (%u,%u)", x, y);
    } else if (op == 0xE5) {
        int x = sign11(words[0] & 0x7FF);
        int y = sign11((words[0] >> 11) & 0x7FF);
        pos = snprintf(out, out_size, "GP0 DrawOffset (%d,%d)", x, y);
    } else if (op == 0xE6) {
        unsigned v = words[0] & 3;
        pos = snprintf(out, out_size, "GP0 MaskBit set=%u check=%u",
                        v & 1, (v >> 1) & 1);
    } else {
        pos = snprintf(out, out_size, "GP0 %02X [%08X]", op, words[0]);
    }

    if (pos >= (int)out_size) out[out_size - 1] = '\0';
}

void decode_gp1(char *out, size_t out_size,
                const uint32_t *words)
{
    uint8_t op = (uint8_t)(words[0] >> 24);
    uint32_t v = words[0] & 0x00FFFFFF;
    int pos = 0;

    switch (op) {
    case 0x00:
        pos = snprintf(out, out_size, "GP1 Reset");
        break;
    case 0x01:
        pos = snprintf(out, out_size, "GP1 ResetCmdBuf");
        break;
    case 0x02:
        pos = snprintf(out, out_size, "GP1 AckIRQ");
        break;
    case 0x03:
        pos = snprintf(out, out_size, "GP1 DispEnable %s",
                        (v & 1) ? "off" : "on");
        break;
    case 0x04:
        pos = snprintf(out, out_size, "GP1 DMADir %u", v & 3);
        break;
    case 0x05: {
        unsigned x = v & 0x3FE;
        unsigned y = (v >> 10) & 0x1FF;
        pos = snprintf(out, out_size, "GP1 DispStart (%u,%u)", x, y);
        break;
    }
    case 0x06: {
        unsigned x1 = v & 0xFFF;
        unsigned x2 = (v >> 12) & 0xFFF;
        pos = snprintf(out, out_size, "GP1 HRange %u-%u", x1, x2);
        break;
    }
    case 0x07: {
        unsigned y1 = v & 0x3FF;
        unsigned y2 = (v >> 10) & 0x3FF;
        pos = snprintf(out, out_size, "GP1 VRange %u-%u", y1, y2);
        break;
    }
    case 0x08: {
        static const unsigned widths[] = { 256, 320, 512, 640 };
        unsigned w_idx = ((v & 3) == 0) ? 0 : ((v & 3) == 1) ? 1 :
                         ((v & 3) == 2) ? 2 : 3;
        unsigned w = (v & 0x40) ? 368 : widths[w_idx];
        unsigned h = (v & 0x04) ? 480 : 240;
        const char *region = (v & 0x08) ? "PAL" : "NTSC";
        const char *depth = (v & 0x10) ? "24bpp" : "15bpp";
        bool interlace = v & 0x20;
        pos = snprintf(out, out_size, "GP1 DispMode %ux%u %s %s%s",
                        w, h, region, depth,
                        interlace ? " interlace" : "");
        break;
    }
    case 0x09:
        pos = snprintf(out, out_size, "GP1 TexDisable %u", v & 1);
        break;
    default:
        if (op >= 0x10 && op <= 0x1F)
            pos = snprintf(out, out_size, "GP1 GetInfo %u", v & 0xF);
        else
            pos = snprintf(out, out_size, "GP1 %02X [%06X]", op, v);
        break;
    }

    if (pos >= (int)out_size) out[out_size - 1] = '\0';
}

void decode_gp0_detail(char *out, size_t out_size,
                       const uint32_t *words, unsigned count)
{
    if (count == 0 || out_size == 0) { if (out_size) out[0] = '\0'; return; }
    uint8_t op = (uint8_t)(words[0] >> 24);
    int pos = 0;

    auto emit = [&](const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (pos >= (int)out_size) return;
        va_list ap;
        va_start(ap, fmt);
        pos += vsnprintf(out + pos, out_size - pos, fmt, ap);
        va_end(ap);
    };

    if (op >= 0x20 && op <= 0x3F) {
        /* Polygon */
        bool tex   = op & 0x04;
        bool shade = op & 0x10;
        unsigned stride = 1 + (shade ? 1 : 0) + (tex ? 1 : 0);
        int nverts = (op & 0x08) ? 4 : 3;

        /* Color(s) */
        emit("Color: %06X", words[0] & 0xFFFFFF);
        if (shade) {
            for (int v = 1; v < nverts; v++) {
                unsigned cidx = 1 + v * stride - (tex ? 2 : 1);
                if (v == 0) cidx = 0;
                if (cidx < count)
                    emit(", %06X", words[cidx] & 0xFFFFFF);
            }
        }
        emit("\n");

        /* Vertices */
        for (int v = 0; v < nverts; v++) {
            unsigned idx = (v == 0) ? 1 : 1 + v * stride;
            if (idx >= count) break;
            int vx = sign11(words[idx] & 0x7FF);
            int vy = sign11((words[idx] >> 16) & 0x7FF);
            emit("V%d: (%d,%d)", v, vx, vy);
            if (tex) {
                unsigned tidx = idx + 1;
                if (tidx < count) {
                    unsigned u = words[tidx] & 0xFF;
                    unsigned v_ = (words[tidx] >> 8) & 0xFF;
                    emit("  UV: (%u,%u)", u, v_);
                    if (v == 0) {
                        unsigned clut = words[tidx] >> 16;
                        unsigned cx = (clut & 0x3F) * 16;
                        unsigned cy = (clut >> 6) & 0x1FF;
                        emit("  CLUT: (%u,%u)", cx, cy);
                    } else if (v == 1) {
                        unsigned tpage = words[tidx] >> 16;
                        unsigned tx = (tpage & 0xF) * 64;
                        unsigned ty = ((tpage >> 4) & 1) * 256;
                        unsigned tp = (tpage >> 7) & 3;
                        const char *depth[] = { "4bpp", "8bpp", "15bpp", "?" };
                        emit("  TPage: (%u,%u) %s", tx, ty, depth[tp]);
                    }
                }
            }
            emit("\n");
        }
    } else if (op >= 0x40 && op <= 0x5F) {
        /* Line */
        bool shade = op & 0x10;
        bool pline = op & 0x08;
        emit("Color: %06X\n", words[0] & 0xFFFFFF);
        unsigned stride = shade ? 2 : 1;
        unsigned maxv = pline ? 16 : 2;
        for (unsigned v = 0; v < maxv; v++) {
            unsigned idx = 1 + v * stride;
            if (idx >= count) break;
            if (pline && words[idx] == 0x55555555) break;
            int vx = sign11(words[idx] & 0x7FF);
            int vy = sign11((words[idx] >> 16) & 0x7FF);
            emit("V%u: (%d,%d)", v, vx, vy);
            if (shade && v > 0) {
                unsigned cidx = idx - 1;
                if (cidx < count)
                    emit("  Color: %06X", words[cidx] & 0xFFFFFF);
            }
            emit("\n");
        }
    } else if (op >= 0x60 && op <= 0x7F) {
        /* Rectangle */
        bool tex = op & 0x04;
        unsigned sz = (op >> 3) & 0x03;
        emit("Color: %06X\n", words[0] & 0xFFFFFF);
        if (count >= 2) {
            int vx = sign11(words[1] & 0x7FF);
            int vy = sign11((words[1] >> 16) & 0x7FF);
            emit("Pos: (%d,%d)\n", vx, vy);
        }
        if (tex && count >= 3) {
            unsigned u = words[2] & 0xFF;
            unsigned v = (words[2] >> 8) & 0xFF;
            unsigned clut = words[2] >> 16;
            unsigned cx = (clut & 0x3F) * 16;
            unsigned cy = (clut >> 6) & 0x1FF;
            emit("UV: (%u,%u)  CLUT: (%u,%u)\n", u, v, cx, cy);
        }
        if (sz == 0) {
            unsigned widx = tex ? 3 : 2;
            if (widx < count) {
                int w = words[widx] & 0x3FF;
                int h = (words[widx] >> 16) & 0x1FF;
                emit("Size: %dx%d\n", w, h);
            }
        }
    } else if (op == 0x02) {
        /* FillRect */
        emit("Color: %06X\n", words[0] & 0xFFFFFF);
        if (count >= 3) {
            int x = words[1] & 0x3F0;
            int y = (words[1] >> 16) & 0x3FF;
            int w = ((words[2] & 0x3FF) + 0xF) & ~0xF;
            int h = (words[2] >> 16) & 0x1FF;
            emit("Pos: (%d,%d)  Size: %dx%d\n", x, y, w, h);
        }
    } else if (op >= 0x80 && op <= 0x9F && count >= 4) {
        int sx = words[1] & 0x3FF, sy = (words[1] >> 16) & 0x3FF;
        int dx = words[2] & 0x3FF, dy = (words[2] >> 16) & 0x3FF;
        int w  = words[3] & 0x3FF, h  = (words[3] >> 16) & 0x1FF;
        if (!w) w = 0x400; if (!h) h = 0x200;
        emit("Src: (%d,%d)  Dst: (%d,%d)  Size: %dx%d\n", sx, sy, dx, dy, w, h);
    } else if (op >= 0xA0 && op <= 0xBF && count >= 3) {
        int x = words[1] & 0x3FF, y = (words[1] >> 16) & 0x3FF;
        int w = words[2] & 0x3FF, h = (words[2] >> 16) & 0x1FF;
        if (!w) w = 0x400; if (!h) h = 0x200;
        emit("Pos: (%d,%d)  Size: %dx%d\n", x, y, w, h);
    } else if (op >= 0xC0 && op <= 0xDF && count >= 3) {
        int x = words[1] & 0x3FF, y = (words[1] >> 16) & 0x3FF;
        int w = words[2] & 0x3FF, h = (words[2] >> 16) & 0x1FF;
        if (!w) w = 0x400; if (!h) h = 0x200;
        emit("Pos: (%d,%d)  Size: %dx%d\n", x, y, w, h);
    }

    if (pos >= (int)out_size) out[out_size - 1] = '\0';
}

static bool psx_gpu_on_event(rd_SubscriptionID sub_id,
                              rd_Event const *event)
{
    if (event->type != RD_EVENT_MISC) return false;
    if (!g_gpu_log_fn) return false;
    if (event->misc.data_size < sizeof(rd_psx_gpu_post)) return false;
    auto *post = (const rd_psx_gpu_post *)event->misc.data;

    unsigned port = post->port;
    unsigned count = post->word_count;
    const uint32_t *words = post->words;

    char line[256];
    if (port == 0)
        decode_gp0(line, sizeof(line), words, count);
    else
        decode_gp1(line, sizeof(line), words);
    g_gpu_log_fn(line);

    (void)sub_id;
    return false;
}

/* ========================================================================
 * GPU post-command trace option
 * ======================================================================== */

static bool psx_gpu_post_on_event(rd_SubscriptionID sub_id,
                                   rd_Event const *event)
{
    if (event->type != RD_EVENT_MISC) return false;
    if (!g_gpu_post_log_fn) return false;
    if (event->misc.data_size < sizeof(rd_psx_gpu_post)) return false;
    auto *post = (const rd_psx_gpu_post *)event->misc.data;

    unsigned port = post->port;
    unsigned count = post->word_count;
    const uint32_t *words = post->words;

    char line[256];
    if (port == 0)
        decode_gp0(line, sizeof(line), words, count);
    else
        decode_gp1(line, sizeof(line), words);

    /* Prefix with "post " to distinguish from pre-command logs */
    char prefixed[280];
    snprintf(prefixed, sizeof(prefixed), "[post] %s", line);
    g_gpu_post_log_fn(prefixed);

    (void)sub_id;
    return false;
}

/* ========================================================================
 * Trace option start/stop
 * ======================================================================== */

static bool psx_trace_option_start(unsigned option_idx, rd_DebuggerIf *dif,
                                   trace_log_fn log_fn)
{
    if (!dif || !dif->v1.subscribe || !dif->v1.system) return false;

    if (option_idx == 0) {
        /* BIOS calls */
        rd_System const *sys = dif->v1.system;
        g_bios_cpu = nullptr;
        for (unsigned i = 0; i < sys->v1.num_cpus; i++) {
            if (sys->v1.cpus[i]->v1.is_main) {
                g_bios_cpu = sys->v1.cpus[i];
                break;
            }
        }
        if (!g_bios_cpu) return false;

        g_bios_log_fn = log_fn;

        static const uint64_t addrs[3] = { 0xA0, 0xB0, 0xC0 };
        for (int i = 0; i < 3; i++) {
            rd_Subscription sub{};
            sub.type = RD_EVENT_EXECUTION;
            sub.execution.cpu = g_bios_cpu;
            sub.execution.type = RD_STEP;
            sub.execution.address_range_begin = addrs[i];
            sub.execution.address_range_end = addrs[i];
            g_bios_subs[i] = dif->v1.subscribe(&sub);
        }
        return true;
    } else if (option_idx == 1) {
        /* GPU commands */
        rd_System const *sys = dif->v1.system;

        /* Find the GP0 and GP1 misc breakpoints from rd_System */
        rd_MiscBreakpoint const *bp_gp0 = nullptr;
        rd_MiscBreakpoint const *bp_gp1 = nullptr;
        for (unsigned i = 0; i < sys->v1.num_break_points; i++) {
            const char *desc = sys->v1.break_points[i]->v1.description;
            if (strcmp(desc, "GP0") == 0) bp_gp0 = sys->v1.break_points[i];
            if (strcmp(desc, "GP1") == 0) bp_gp1 = sys->v1.break_points[i];
        }
        if (!bp_gp0 || !bp_gp1) return false;

        g_gpu_log_fn = log_fn;

        rd_Subscription sub{};
        sub.type = RD_EVENT_MISC;
        sub.misc.breakpoint = bp_gp0;
        g_gpu_subs[0] = dif->v1.subscribe(&sub);

        sub.misc.breakpoint = bp_gp1;
        g_gpu_subs[1] = dif->v1.subscribe(&sub);

        return true;
    } else if (option_idx == 2) {
        /* GPU post-commands */
        rd_System const *sys = dif->v1.system;

        rd_MiscBreakpoint const *bp_post = nullptr;
        for (unsigned i = 0; i < sys->v1.num_break_points; i++) {
            const char *desc = sys->v1.break_points[i]->v1.description;
            if (strcmp(desc, "GPU Post") == 0) bp_post = sys->v1.break_points[i];
        }
        if (!bp_post) return false;

        g_gpu_post_log_fn = log_fn;

        rd_Subscription sub{};
        sub.type = RD_EVENT_MISC;
        sub.misc.breakpoint = bp_post;
        g_gpu_post_sub = dif->v1.subscribe(&sub);

        return true;
    }

    return false;
}

static void psx_trace_option_stop(unsigned option_idx, rd_DebuggerIf *dif)
{
    if (option_idx == 0) {
        if (dif && dif->v1.unsubscribe) {
            for (int i = 0; i < 3; i++) {
                if (g_bios_subs[i] >= 0) {
                    dif->v1.unsubscribe(g_bios_subs[i]);
                    g_bios_subs[i] = -1;
                }
            }
        }
        g_bios_log_fn = nullptr;
        g_bios_cpu = nullptr;
    } else if (option_idx == 1) {
        if (dif && dif->v1.unsubscribe) {
            for (int i = 0; i < 2; i++) {
                if (g_gpu_subs[i] >= 0) {
                    dif->v1.unsubscribe(g_gpu_subs[i]);
                    g_gpu_subs[i] = -1;
                }
            }
        }
        g_gpu_log_fn = nullptr;
    } else if (option_idx == 2) {
        if (dif && dif->v1.unsubscribe) {
            if (g_gpu_post_sub >= 0) {
                dif->v1.unsubscribe(g_gpu_post_sub);
                g_gpu_post_sub = -1;
            }
        }
        g_gpu_post_log_fn = nullptr;
    }
}

/* ========================================================================
 * Check if a subscription ID belongs to PSX tracing
 * ======================================================================== */

static bool psx_trace_is_sub(rd_SubscriptionID sub_id) {
    for (int i = 0; i < 3; i++)
        if (g_bios_subs[i] >= 0 && g_bios_subs[i] == sub_id)
            return true;
    for (int i = 0; i < 2; i++)
        if (g_gpu_subs[i] >= 0 && g_gpu_subs[i] == sub_id)
            return true;
    if (g_gpu_post_sub >= 0 && g_gpu_post_sub == sub_id)
        return true;
    return false;
}

/* ========================================================================
 * Trace option descriptors
 * ======================================================================== */

static const TraceOption psx_trace_options[] = {
    { "BIOS calls" },
    { "GPU commands" },
    { "GPU post-commands" },
};

/* ========================================================================
 * Dispatch trace events
 * ======================================================================== */

static bool psx_trace_on_event_dispatch(rd_SubscriptionID sub_id,
                                         rd_Event const *event)
{
    /* BIOS call? */
    for (int i = 0; i < 3; i++)
        if (g_bios_subs[i] >= 0 && g_bios_subs[i] == sub_id)
            return psx_trace_on_event(sub_id, event);
    /* GPU command? */
    for (int i = 0; i < 2; i++)
        if (g_gpu_subs[i] >= 0 && g_gpu_subs[i] == sub_id)
            return psx_gpu_on_event(sub_id, event);
    /* GPU post-command? */
    if (g_gpu_post_sub >= 0 && g_gpu_post_sub == sub_id)
        return psx_gpu_post_on_event(sub_id, event);
    return false;
}

/* ========================================================================
 * System descriptor
 * ======================================================================== */

extern const Sys sys_psx = {
    "psx",
    psx_int_names, 11,
    psx_trace_options, 3,
    psx_trace_option_start,
    psx_trace_option_stop,
    psx_trace_is_sub,
    psx_trace_on_event_dispatch,
};

} // namespace sys
