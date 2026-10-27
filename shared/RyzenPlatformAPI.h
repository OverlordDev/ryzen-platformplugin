/*
 * RyzenPlatformAPI.h — ABI между kext и userspace-демоном.
 *
 * Правила:
 *   - только POD, фиксированные размеры, без указателей;
 *   - abi_version проверяется на каждой границе;
 *   - struct_size позволяет добавлять поля в конец без ломания старых клиентов;
 *   - kext всегда клампит и проверяет read-back, даже если демон прислал чушь.
 */

#ifndef RYZEN_PLATFORM_API_H
#define RYZEN_PLATFORM_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Terminology: every "core" field in this ABI is a LOGICAL CPU index, i.e. the
 * value of cpu_number() inside the kext (0..core_count-1). With SMT on, two
 * consecutive indices are usually the two threads of one physical core, but the
 * kext does not promise that; the daemon owns that assumption (--smt-stride).
 *
 * How to talk to the kext (IOServiceOpen type 0, root only, one client at a time):
 *   - getters take ONE scalar input: the client's abi_version, plus the core
 *     index as a SECOND scalar for readState;
 *   - applyPerf and heartbeat take a struct input that carries abi_version;
 *   - all structs are returned/accepted at EXACTLY sizeof() of this header.
 */
#define RYZEN_PLATFORM_ABI_VERSION 2u
#define RYZEN_PLATFORM_MAX_CORES 32u
#define RYZEN_PLATFORM_ERROR_MSG_LEN 128u

/* ------------------------------------------------------------------------- */
/* Selectors                                                                  */
/* ------------------------------------------------------------------------- */

typedef enum {
    kRyzenSelectorGetTopology   = 0,
    kRyzenSelectorReadState     = 1,
    kRyzenSelectorApplyPerf     = 2,
    kRyzenSelectorResetAuto     = 3,
    kRyzenSelectorHeartbeat     = 4,
    kRyzenSelectorGetLastError  = 5,
    kRyzenSelectorGetStatus     = 6,
    kRyzenSelectorCount         = 7,
} ryzen_selector_t;

/* ------------------------------------------------------------------------- */
/* Flags                                                                      */
/* ------------------------------------------------------------------------- */

enum {
    RYZEN_FLAG_CPPC_SUPPORTED   = 1u << 0,
    RYZEN_FLAG_CPPC_ENABLED     = 1u << 1,
    RYZEN_FLAG_WATCHDOG_ARMED   = 1u << 2,
    RYZEN_FLAG_THERMAL_GUARD    = 1u << 3,
    RYZEN_FLAG_BOOT_ARG_DISABLED= 1u << 4,
    RYZEN_FLAG_OVERRIDE_ACTIVE  = 1u << 5,
    RYZEN_FLAG_SLEEPING         = 1u << 6,
    RYZEN_FLAG_WRITE_ENABLED    = 1u << 7,   /* boot-arg ryzenplatform_write=1 was given */
    RYZEN_FLAG_LOCKOUT          = 1u << 8,   /* kext refuses applyPerf until resetAuto */
};

/* ------------------------------------------------------------------------- */
/* Errors                                                                     */
/* ------------------------------------------------------------------------- */

typedef enum {
    RYZEN_OK             = 0,
    RYZEN_E_ABI          = 1,
    RYZEN_E_UNSUPPORTED  = 2,
    RYZEN_E_RANGE        = 3,
    RYZEN_E_READBACK     = 4,
    RYZEN_E_THERMAL      = 5,
    RYZEN_E_WATCHDOG     = 6,
    RYZEN_E_INTERNAL     = 7,
    RYZEN_E_DISABLED     = 8,   /* writes not enabled: boot with ryzenplatform_write=1 */
    RYZEN_E_LOCKOUT      = 9,   /* a safety stop is latched; call resetAuto once it is cleared */
} ryzen_error_code_t;

/* ------------------------------------------------------------------------- */
/* Topology                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t core_count;
    uint32_t cppc_supported;
    uint32_t cppc_enabled;
    uint32_t preferred_core;
    uint32_t highest_perf;
    uint32_t nominal_perf;
    uint32_t lowest_perf;
    uint32_t highest_freq_mhz;
    uint32_t nominal_freq_mhz;
    uint32_t lowest_freq_mhz;
    uint32_t flags;
    uint32_t reserved[3];
} ryzen_topology_t;

/* ------------------------------------------------------------------------- */
/* Per-core state                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t core;
    uint32_t desired_perf;
    uint32_t max_perf;
    uint32_t min_perf;
    uint32_t epp;
    uint32_t effective_freq_mhz;
    uint32_t temp_millic;
    uint32_t package_power_mw;
    uint32_t flags;
    uint32_t reserved[3];
} ryzen_core_state_t;

/* ------------------------------------------------------------------------- */
/* Request / apply                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t core;
    uint32_t desired_perf;
    uint32_t max_perf;
    uint32_t min_perf;
    uint32_t epp;
    uint32_t ttl_ms;        /* 0 = до следующего запроса / heartbeat */
} ryzen_perf_request_t;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t count;         /* сколько элементов req[] валидно */
    uint32_t flags;
    ryzen_perf_request_t req[RYZEN_PLATFORM_MAX_CORES];
} ryzen_apply_t;

/* ------------------------------------------------------------------------- */
/* Heartbeat                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t sequence;
    uint32_t daemon_pid;
    uint32_t flags;
    uint32_t reserved[3];
} ryzen_heartbeat_t;

/* ------------------------------------------------------------------------- */
/* Status (everything the daemon needs for its own safety checks)             */
/* ------------------------------------------------------------------------- */

#define RYZEN_TEMP_UNKNOWN (-2147483647 - 1)   /* INT32_MIN, spelled out: no <limits.h> in the kernel */

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t flags;               /* RYZEN_FLAG_* */
    uint32_t guard_level;         /* 0 none, 1 cap at nominal, 2 all lowest, 3 reset + lockout */
    int32_t  tctl_millic;         /* RYZEN_TEMP_UNKNOWN if the sensor did not answer */
    uint32_t heartbeat_age_ms;    /* since the last heartbeat; meaningful while an override is active */
    uint32_t watchdog_resets;     /* times the kext reverted on its own because heartbeats stopped */
    uint32_t readback_failures;   /* writes that did not read back; any non-zero value is a red flag */
    uint32_t smn_retries;         /* foreign SMN index/data interleaves caught */
    uint32_t cpu_count;
    uint32_t reserved[4];
} ryzen_status_t;

/* ------------------------------------------------------------------------- */
/* Last error                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t code;          /* ryzen_error_code_t */
    uint32_t core;
    uint64_t timestamp_ns;
    char message[RYZEN_PLATFORM_ERROR_MSG_LEN];
} ryzen_error_t;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static inline int ryzen_abi_compatible(uint32_t client_abi) {
    return client_abi == RYZEN_PLATFORM_ABI_VERSION;
}

static inline int ryzen_core_valid(uint32_t core, uint32_t core_count) {
    return core < core_count && core < RYZEN_PLATFORM_MAX_CORES;
}

#ifdef __cplusplus
}
#endif

#endif /* RYZEN_PLATFORM_API_H */
