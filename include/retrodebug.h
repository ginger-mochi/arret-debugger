/*
 * retrodebug.h
 * based on hcdebug.h by leiradel
 *
 * Everything starts at rd_DebuggerIf, so please see that struct first.
*/

#ifndef RETRO_DEBUG__
#define RETRO_DEBUG__

#include <stdbool.h>
#include <stdint.h>

#define RD_API_VERSION 1

/* Watchpoint operations */
#define RD_MEMORY_READ (1 << 0)
#define RD_MEMORY_WRITE (1 << 1)

/* IO watchpoint operations */
#define RD_IO_READ (1 << 0)
#define RD_IO_WRITE (1 << 1)

/* Event types */
typedef enum {
    RD_EVENT_TICK = 0,
    RD_EVENT_EXECUTION = 1,
    RD_EVENT_INTERRUPT = 2,
    RD_EVENT_MEMORY = 3,
    RD_EVENT_REG = 4,
    RD_EVENT_IO = 5,
    RD_EVENT_MISC = 6
}
rd_EventType;

/* Subscription ID. Helps identify subscriber, and also allows unsubscribing from an event. A negative ID indicates an error.
   IDs are not necessarily consecutive, and an ID may be re-used only after unsubscribing. Otherwise, IDs are unique, even
   between different event types. (The core might implement this by using some bits of the event ID to indicate the event type.) */
typedef int64_t rd_SubscriptionID;

typedef enum {
    /* Report all execution events */
    RD_STEP,
    
    /* As above, but if an interrupt occurs, temporarily disable until returned from interrupt */
    RD_STEP_SKIP_INTERRUPT,
    
    /* As above, but if a subroutine is invoked, temporarily disable until returned from subroutine */
    RD_STEP_CURRENT_SUBROUTINE,

    /* Initially disabled; only enabled after returning from the current subroutine. */
    RD_STEP_OUT,
}
rd_ExecutionType;

typedef struct rd_MiscBreakpoint {
    struct {
        /* Breakpoint info */
        char const* description;
    }
    v1;
}
rd_MiscBreakpoint;

typedef struct rd_Memory rd_Memory;

/*
 * Describes one region of a memory map.  Entries must be consecutive: each
 * entry's base_addr equals the previous entry's base_addr + size.
 * size must not be 0.
 *
 * source     - optional backing memory object (e.g. the full ROM).  NULL if
 *              this region has no separate source or the core doesn't expose one.
 * source_base_addr - offset within source that this window maps to.
 *              Must be 0 when source is NULL.
 * bank       - optional bank number.  Negative if unused/undefined.
 */
typedef struct rd_MemoryMap {
    uint64_t base_addr;
    uint64_t size;
    rd_Memory const* source;
    uint64_t source_base_addr;
    int64_t  bank;
}
rd_MemoryMap;

struct rd_Memory {
    struct {
        /* Memory info */
        char const* id;
        char const* description;
        unsigned alignment; /* in bytes */
        uint64_t base_address;
        uint64_t size;

        /* Supported breakpoints not covered by specific functions */
        rd_MiscBreakpoint const* const* break_points;
        unsigned num_break_points;

        /* Reads a byte from an address.
         * If side_effects is false, the core must guarantee no side effects
         * (e.g. from IO register access) occur.  If a side-effect-free read
         * is not possible at this address, the core should return 0.
         * peek must never cause memory subscriptions to fire. */
        uint8_t (*peek)(struct rd_Memory const* self, uint64_t address, bool side_effects);

        /* poke can be null for read-only memory but all memory should be
         * writeable to allow patching. poke can be non-null and still not
         * change the value, i.e. for the main memory region when the address
         * is in ROM. If poke succeeds to write to the given address, it
         * returns a value different from 0 (true).
         * poke must never cause memory subscriptions to fire. */
        int (*poke)(struct rd_Memory const* self, uint64_t address, uint8_t value);

        /*
         * Optional memory map.  Both pointers must be non-NULL or both NULL.
         * The caller allocates the array using the count returned by
         * get_memory_map_count(), then passes it to get_memory_map() to fill.
         */
        unsigned (*get_memory_map_count)(struct rd_Memory const* self);
        void (*get_memory_map)(struct rd_Memory const* self, rd_MemoryMap *out);

        /* Optional. Returns true if there is banking possible at the given address,
         * and if so, populates the given single rd_MemoryMap struct (if it is not NULL)
         * to specify where the given address would point if the given bank were loaded.
         */
        bool (*get_bank_address)(struct rd_Memory const* self, uint64_t address, int64_t bank, rd_MemoryMap* out);
    }
    v1;
};

typedef struct rd_Cpu {
    struct {
        /* CPU info */
        char const* id;
        char const* description;
        unsigned type;
        int is_main; /* only one CPU can be the main CPU */

        /* Memory region that is CPU addressable */
        rd_Memory const* memory_region;

        /* Supported breakpoints not covered by specific functions */
        rd_MiscBreakpoint const* const* break_points;
        unsigned num_break_points;

        /* Registers, return true on set_register to signal a successful write */
        uint64_t (*get_register)(struct rd_Cpu const* self, unsigned reg);
        int (*set_register)(struct rd_Cpu const* self, unsigned reg, uint64_t value);
    }
    v1;
}
rd_Cpu;

typedef struct rd_System {
    struct {
        /* Common system name, lower case -- e.g. "nes", "gb", "gbc", "megadrive". Allows the front-end to identify the system type, so if there are other cores that implement the same system, follow their lead. */
        char const* description;

        /* CPUs available in the system */
        rd_Cpu const* const* cpus;
        unsigned num_cpus;

        /* Memory regions that aren't addressable by any of the CPUs on the system */
        rd_Memory const* const* memory_regions;
        unsigned num_memory_regions;

        /* Supported breakpoints not covered by specific functions */
        rd_MiscBreakpoint const* const* break_points;
        unsigned num_break_points;

        /* Write human-readable info about the loaded content (cartridge header,
           mapper, game title, checksum, etc.) into outbuff.
           outbuff may be NULL and outsize may be 0.
           Returns the number of chars that would be written for the complete output. */
        int (*get_content_info)(char* outbuff, int outsize);
    }
    v1;
}
rd_System;

/* Informs the front-end that a CPU is about to execute an instruction at the given address */
typedef struct rd_ExecutionEvent {
    rd_Cpu const* cpu;
    uint64_t address;
}
rd_ExecutionEvent;

/* Informs the front-end that an interrupt was served */
typedef struct rd_InterruptEvent {
    rd_Cpu const* cpu;
    
    /* Identifies the type of interrupt. Meaning depends on CPU model */
    unsigned kind;
    
    /* Address of the next instruction to be executed when returning from interrupt */
    uint64_t return_address;
    
    /* New value of the program counter (in general, the start of the interrupt vector) */
    uint64_t vector_address;
}
rd_InterruptEvent;

/* Informs the front-end that a memory location is about to be read from or written to */
typedef struct rd_MemoryWatchpointEvent {
    rd_Memory const* memory;
    uint64_t address;
    uint8_t operation;
    uint8_t value;
}
rd_MemoryWatchpointEvent;

/* Informs the front-end that a register is about to have its value changed */
typedef struct rd_RegisterWatchpointEvent {
    rd_Cpu const* cpu;
    unsigned reg;
    uint64_t new_value;
}
rd_RegisterWatchpointEvent;

/* Informs the front-end that an IO port is about to be read from or written to */
typedef struct rd_IoWatchpointEvent {
    rd_Cpu const* cpu;
    uint64_t address;
    uint8_t operation;
    uint64_t value;
}
rd_IoWatchpointEvent;

/* Informs the front-end that a misc breakpoint was hit */
typedef struct rd_MiscBreakpointEvent {
    rd_MiscBreakpoint const* breakpoint;
    uint64_t args[4];
}
rd_MiscBreakpointEvent;

/* Tagged union over all hc Event types */
typedef struct rd_Event {
    rd_EventType type;

    /* True if the core can halt execution and return from retro_run()
     * immediately.  When true, the frontend may return true from
     * handle_event to request that the core break its run loop and
     * return from retro_run(); remaining events should be postponed
     * to the next retro_run() invocation.
     * When false, the core cannot break cleanly at this point, and
     * the frontend must fall back to blocking the core's thread
     * within this handler if it wishes to pause execution. */
    bool can_halt;

    union {
        rd_ExecutionEvent execution;
        rd_InterruptEvent interrupt;
        rd_MemoryWatchpointEvent memory;
        rd_RegisterWatchpointEvent reg;
        rd_IoWatchpointEvent io;
        rd_MiscBreakpointEvent misc;
    };
}
rd_Event;

/* Tells the core to report certain execution events. Note that the core should implicitly include the
   current stack depth and/or subroutine being executed in the context of this subscription.
 *
 * address_range_begin / address_range_end define the PC range of interest.
 * Typical values:
 *   - Broad (all addresses): begin=0, end=UINT64_MAX
 *   - Single address (breakpoint): begin=addr, end=addr
 * Cores may reject ranges that are neither broad nor single-address. */
typedef struct rd_ExecutionSubscription {
    rd_Cpu const* cpu;
    rd_ExecutionType type;
    uint64_t address_range_begin;
    uint64_t address_range_end;
}
rd_ExecutionSubscription;

/* Tells the core to report certain interrupt events */
typedef struct rd_InterruptSubscription {
    rd_Cpu const* cpu;
    unsigned kind;
}
rd_InterruptSubscription;

/* Tells the core to report certain memory access events */
typedef struct rd_MemoryWatchpointSubscription {
    rd_Memory const* memory;
    uint64_t address_range_begin;
    uint64_t address_range_end;
    uint8_t operation;
}
rd_MemoryWatchpointSubscription;

/* Tells the core to report certain register change events */
typedef struct rd_RegisterWatchpointSubscription {
    rd_Cpu const* cpu;
    unsigned reg;
}
rd_RegisterWatchpointSubscription;

/* Tells the core to report when an IO port is accessed  */
typedef struct rd_IoWatchpointSubscription {
    rd_Cpu const* cpu;
    uint64_t address_range_begin;
    uint64_t address_range_end;
    uint8_t operation;
}
rd_IoWatchpointSubscription;

/* Tells the core to report a misc breakpoint event */
typedef struct rd_MiscBreakpointSubscription {
    rd_MiscBreakpoint const* breakpoint;
}
rd_MiscBreakpointSubscription;

/* Informs the core that a particular type of event should be reported (via handle_event()) */
typedef struct rd_Subscription {
    rd_EventType type;
    
    union {
        rd_ExecutionSubscription execution;
        rd_InterruptSubscription interrupt;
        rd_MemoryWatchpointSubscription memory;
        rd_RegisterWatchpointSubscription reg;
        rd_IoWatchpointSubscription io;
        rd_MiscBreakpointSubscription misc;
    };
}
rd_Subscription;

/*
 * Debug interface. Shared between the core and frontend.
 * Members which are const are initialized by the frontend.
 *
 * The frontend allocates rd_DebuggerIf, fills in the const fields
 * (frontend_api_version, user_data, handle_event), and passes it to
 * rd_set_debugger().
 *
 * During rd_set_debugger() the core fills in:
 *   - core_api_version
 *   - v1.system   (pointer to the core's rd_System)
 *   - v1.subscribe / v1.unsubscribe
 *
 * The rd_System pointer itself is set during rd_set_debugger(), but its
 * contents (CPUs, memory regions, sizes) may continue to be updated by
 * the core until retro_load_game() returns.  For example, memory region
 * sizes and the set of available regions may depend on the loaded
 * content (ROM header, MBC type, CGB mode, etc.).
 *
 * The frontend must not cache or act on system topology (region lists,
 * sizes, memory map entries) until after retro_load_game() has returned
 * successfully. For cores that do not load content, initialization should be immediate.
 */
typedef struct rd_DebuggerIf {
    unsigned const frontend_api_version;
    unsigned core_api_version;

    struct {
        /* The emulated system */
        rd_System const* system;

        /* A front-end user-defined data */
        void* const user_data;

        /* Handles an event from the core.
         * Return true to request that the core halt execution.
         * The return value is only meaningful when can_halt is true;
         * the core must ignore it when can_halt is false.
         * If can_halt is true and the handler returns true, the core
         * will break its run loop and return from retro_run() cleanly.
         * If can_halt is false, the core cannot break at this point
         * and the frontend must block the calling thread within this
         * handler if it wishes to pause execution.
         *
         * The frontend must NOT save or load state during this handler.
         * State operations are only safe after retro_run() has returned.
         *
         * The core should be prepared for the program counter to have been
         * modified when this handler returns.
         *
         * The frontend may call subscribe() and unsubscribe() from within
         * this handler (e.g. to add or remove breakpoints, or to clean up
         * temporary subscriptions).  Core implementations must ensure that
         * modifying the subscription set during event dispatch is safe. */
        bool (* const handle_event)(void* frontend_user_data, rd_SubscriptionID subscription_id, rd_Event const* event);

        /* Tells the core to report certain events. Returns negative value if not supported or if an error occurred.
         * Must be safe to call from within handle_event (see above). */
        rd_SubscriptionID (* subscribe)(rd_Subscription const* subscription);
        /* Must be safe to call from within handle_event (see above). */
        void (* unsubscribe)(rd_SubscriptionID subscription_id);
    }
    v1;
}
rd_DebuggerIf;

typedef void (*rd_Set)(rd_DebuggerIf* const debugger_if);

#define RD_MAKE_CPU_TYPE(id, version) ((id) << 16 | (version))
#define RD_CPU_API_VERSION(type) ((type) & 0xffffU)

/* Supported CPUs in API version 1 */
#define RD_CPU_Z80 RD_MAKE_CPU_TYPE(0, 1)

#define RD_Z80_A 0
#define RD_Z80_F 1
#define RD_Z80_BC 2
#define RD_Z80_DE 3
#define RD_Z80_HL 4
#define RD_Z80_IX 5
#define RD_Z80_IY 6
#define RD_Z80_AF2 7
#define RD_Z80_BC2 8
#define RD_Z80_DE2 9
#define RD_Z80_HL2 10
#define RD_Z80_I 11
#define RD_Z80_R 12
#define RD_Z80_SP 13
#define RD_Z80_PC 14
#define RD_Z80_IFF 15
#define RD_Z80_IM 16
#define RD_Z80_WZ 17

#define RD_Z80_NUM_REGISTERS 18

#define RD_Z80_INT 0
#define RD_Z80_NMI 1

#define RD_CPU_6502 RD_MAKE_CPU_TYPE(1, 1)

#define RD_6502_A 0
#define RD_6502_X 1
#define RD_6502_Y 2
#define RD_6502_S 3
#define RD_6502_PC 4
#define RD_6502_P 5

#define RD_6502_NUM_REGISTERS 6

#define RD_6502_NMI 0
#define RD_6502_IRQ 1

#define RD_CPU_65816 RD_MAKE_CPU_TYPE(2, 1)

#define RD_65816_A 0
#define RD_65816_X 1
#define RD_65816_Y 2
#define RD_65816_S 3
#define RD_65816_PC 4
#define RD_65816_P 5
#define RD_65816_DB 6
#define RD_65816_D 7
#define RD_65816_PB 8
#define RD_65816_EMU 9 /* 'hidden' 1-bit register, set to 1 in emulation mode, 0 in native mode */

#define RD_65816_NUM_REGISTERS 10

#define RD_CPU_R3000A RD_MAKE_CPU_TYPE(3, 1)

#define RD_R3000A_R0 0
#define RD_R3000A_AT 1
#define RD_R3000A_V0 2
#define RD_R3000A_V1 3
#define RD_R3000A_A0 4
#define RD_R3000A_A1 5
#define RD_R3000A_A2 6
#define RD_R3000A_A3 7
#define RD_R3000A_T0 8
#define RD_R3000A_T1 9
#define RD_R3000A_T2 10
#define RD_R3000A_T3 11
#define RD_R3000A_T4 12
#define RD_R3000A_T5 13
#define RD_R3000A_T6 14
#define RD_R3000A_T7 15
#define RD_R3000A_S0 16
#define RD_R3000A_S1 17
#define RD_R3000A_S2 18
#define RD_R3000A_S3 19
#define RD_R3000A_S4 20
#define RD_R3000A_S5 21
#define RD_R3000A_S6 22
#define RD_R3000A_S7 23
#define RD_R3000A_T8 24
#define RD_R3000A_T9 25
#define RD_R3000A_K0 26
#define RD_R3000A_K1 27
#define RD_R3000A_GP 28
#define RD_R3000A_SP 29
#define RD_R3000A_FP 30
#define RD_R3000A_RA 31
#define RD_R3000A_PC 32
#define RD_R3000A_LO 33
#define RD_R3000A_HI 34

#define RD_R3000A_NUM_REGISTERS 35

/* LR35902 (Game Boy CPU) - Sharp SM83-based CPU used in Game Boy/Color */
#define RD_CPU_LR35902 RD_MAKE_CPU_TYPE(4, 1)

/* LR35902 registers - 8-bit registers can be combined into 16-bit pairs */
#define RD_LR35902_A 0   /* Accumulator */
#define RD_LR35902_F 1   /* Flags: Z N H C (bits 7-4), bits 3-0 always 0 */
#define RD_LR35902_B 2
#define RD_LR35902_C 3
#define RD_LR35902_D 4
#define RD_LR35902_E 5
#define RD_LR35902_H 6
#define RD_LR35902_L 7
#define RD_LR35902_SP 8  /* Stack Pointer (16-bit) */
#define RD_LR35902_PC 9  /* Program Counter (16-bit) */
/* Combined 16-bit register pairs for convenience */
#define RD_LR35902_AF 10
#define RD_LR35902_BC 11
#define RD_LR35902_DE 12
#define RD_LR35902_HL 13
/* Interrupt state */
#define RD_LR35902_IME 14 /* Interrupt Master Enable */

#define RD_LR35902_NUM_REGISTERS 15

/* LR35902 interrupt types */
#define RD_LR35902_INT_VBLANK 0  /* V-Blank interrupt (INT 0x40) */
#define RD_LR35902_INT_STAT 1    /* LCD STAT interrupt (INT 0x48) */
#define RD_LR35902_INT_TIMER 2   /* Timer interrupt (INT 0x50) */
#define RD_LR35902_INT_SERIAL 3  /* Serial interrupt (INT 0x58) */
#define RD_LR35902_INT_JOYPAD 4  /* Joypad interrupt (INT 0x60) */

/* LR35902 Flag bits (in F register) */
#define RD_LR35902_FLAG_Z 0x80  /* Zero flag */
#define RD_LR35902_FLAG_N 0x40  /* Subtract flag */
#define RD_LR35902_FLAG_H 0x20  /* Half-carry flag */
#define RD_LR35902_FLAG_C 0x10  /* Carry flag */

#endif /* RETRO_DEBUG__ */

