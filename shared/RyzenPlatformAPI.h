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

#define RYZEN_PLATFORM_ABI_VERSION 1u
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
