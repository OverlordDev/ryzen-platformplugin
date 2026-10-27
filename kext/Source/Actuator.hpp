/*
 * Actuator.hpp - the only code path that ever writes MSR_AMD_CPPC_REQ.
 *
 * Contract (see docs/SAFETY.md):
 *   - nothing is written unless the kext was booted with ryzenplatform_write=1;
 *   - before the first write, every CPU must report CPPC enabled and sane caps;
 *   - the firmware's own CPPC_REQ is captured per CPU first and is what
 *     "auto" means: reset restores that exact value, not a guess;
 *   - every request is clamped to that CPU's [lowest, highest] and every write
 *     is read back; a mismatch restores the firmware value and latches a lockout;
 *   - while an override is active the daemon must heartbeat; silence for
 *     150 ms restores the firmware value (watchdog, lives here, not in userspace);
 *   - thermal guard here is independent of the daemon: 90 C caps at nominal,
 *     98 C pins everything to lowest, 100 C restores and latches a lockout,
 *     and a sensor that does not answer counts as "cap at nominal", never as OK.
 *
 * All entry points are thread-safe: they run from the user client thread, the
 * watchdog timer and the sampler.
 */
#ifndef RYZEN_ACTUATOR_HPP
#define RYZEN_ACTUATOR_HPP

#include <stdint.h>
#include "RyzenPlatformAPI.h"

/* Locks only. Call once, before anything uses ryzenRendezvous(). */
bool actInit(bool cppcSupported, bool writeEnabled);
/* After the CPU count is known (dense 0..n-1). */
void actSetCpuCount(uint32_t n);
/* Restores firmware values and frees locks. Idempotent. */
void actShutdown(void);

int  actTopology(ryzen_topology_t *out);                 /* ryzen_error_code_t */
int  actReadState(uint32_t cpu, ryzen_core_state_t *out);
int  actApply(const ryzen_apply_t *in);
int  actResetAuto(const char *why, bool userRequested);  /* userRequested may clear a lockout */
int  actHeartbeat(const ryzen_heartbeat_t *hb);
void actStatus(ryzen_status_t *out);
void actLastError(ryzen_error_t *out);

/* True while an override is active, i.e. while actTick() must keep being called. */
bool actNeedsTick(void);
/* Watchdog, per-CPU TTL and thermal guard. Call every ~50 ms while actNeedsTick(). */
void actTick(void);

/* Sleep/wake hooks. The daemon also resets before sleep; these are the kext-side backstop. */
void actPrepareForSleep(void);
void actWake(void);

#endif /* RYZEN_ACTUATOR_HPP */
