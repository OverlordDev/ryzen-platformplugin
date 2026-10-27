/*
 * KernelDecls.hpp - kernel symbols that are exported but live outside the
 * public headers. mp_rendezvous_no_intrs and cpu_number are the same set that
 * RyzenHV already links against, so those two are known to resolve.
 *
 * UNVERIFIED (written from memory, check on the first real build; see
 * docs/HANDOFF.md): wrmsr_carefully, clock_get_uptime, absolutetime_to_nanoseconds.
 * If the SDK headers already declare them, these redeclarations must match
 * exactly or be deleted.
 */
#ifndef RYZEN_PLATFORM_KERNELDECLS_HPP
#define RYZEN_PLATFORM_KERNELDECLS_HPP

#include <stdint.h>

extern "C" {
/* Runs `action` on every CPU with interrupts off. */
void mp_rendezvous_no_intrs(void (*action)(void *), void *arg);
/* Index of the CPU we are running on right now. */
int  cpu_number(void);

/* Like rdmsr_carefully: returns non-zero instead of taking a #GP. */
int  wrmsr_carefully(uint32_t msr, uint32_t lo, uint32_t hi);

/* Monotonic time. Mach absolute time stops during sleep on Intel Macs. */
void clock_get_uptime(uint64_t *result);
void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
}

#endif /* RYZEN_PLATFORM_KERNELDECLS_HPP */
