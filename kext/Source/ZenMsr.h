/*
 * ZenMsr.h - AMD Family 17h/19h MSRs and bit fields we care about.
 *
 * Sources: AMD PPR for Cezanne (Family 19h, Model 50h), "CPPC" chapter;
 * Linux arch/x86/kernel/cpu/amd.c and drivers/cpufreq/amd-pstate.c.
 */
#ifndef ZEN_MSR_H
#define ZEN_MSR_H

#include <stdint.h>

/* ---- CPPC (Collaborative Processor Performance Control) ---- */
#define MSR_AMD_CPPC_CAP1    0xC00102B0u   /* per-core capabilities, read-only  */
#define MSR_AMD_CPPC_ENABLE  0xC00102B1u   /* bit 0: CPPC on                    */
#define MSR_AMD_CPPC_CAP2    0xC00102B2u   /* per-core capabilities, read-only  */
#define MSR_AMD_CPPC_REQ     0xC00102B3u   /* the request we write              */
#define MSR_AMD_CPPC_STATUS  0xC00102B4u   /* what the SMU actually applied     */

/* CAP1: four 8-bit performance values. "Perf" is linear in frequency between
 * LOWEST and HIGHEST; the SMU maps it to a real P-state. */
#define CPPC_CAP1_LOWEST(x)     ((uint32_t)((x)       & 0xFFu))
#define CPPC_CAP1_LOWEST_NL(x)  ((uint32_t)(((x) >> 8)  & 0xFFu))
#define CPPC_CAP1_NOMINAL(x)    ((uint32_t)(((x) >> 16) & 0xFFu))
#define CPPC_CAP1_HIGHEST(x)    ((uint32_t)(((x) >> 24) & 0xFFu))

/* REQ: min / max / desired perf + energy performance preference. */
#define CPPC_REQ_MIN(x)         ((uint32_t)((x)       & 0xFFu))
#define CPPC_REQ_MAX(x)         ((uint32_t)(((x) >> 8)  & 0xFFu))
#define CPPC_REQ_DESIRED(x)     ((uint32_t)(((x) >> 16) & 0xFFu))
#define CPPC_REQ_EPP(x)         ((uint32_t)(((x) >> 24) & 0xFFu))

/* STATUS */
#define CPPC_STATUS_ENABLED     (1ull << 0)

/* ---- P-states ---- */
#define MSR_AMD_PSTATE_STATUS 0xC0010063u   /* bits 2:0 = current P-state index  */
#define MSR_AMD_PSTATE_DEF0   0xC0010064u   /* P0..P7 definitions follow        */

#define PSTATE_DEF_FID(x)       ((uint32_t)((x)       & 0xFFu))
#define PSTATE_DEF_DID(x)       ((uint32_t)(((x) >> 8)  & 0x3Fu))
#define PSTATE_DEF_VID(x)       ((uint32_t)(((x) >> 14) & 0x1FFu))

/* ---- SMN (System Management Network), reached through D0F0 0x60/0x64 ---- */
#define SMN_THM_TCON_CUR_TMP   0x00059800u  /* bits 31:21 = Tctl in 1/8 C        */
#define SMN_TCTL_SHIFT         21
#define SMN_TCTL_MASK          0x7FFu
#define SMN_TCTL_MDEG(v)       ((uint32_t)(((v) >> SMN_TCTL_SHIFT) & SMN_TCTL_MASK) * 125u)

#endif /* ZEN_MSR_H */
