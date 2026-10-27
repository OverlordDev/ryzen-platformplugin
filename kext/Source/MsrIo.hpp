/*
 * MsrIo.hpp - the only places in the kext that touch an MSR.
 *
 * Both directions go through the *_carefully variants, which return non-zero
 * instead of taking a #GP when the MSR does not exist or refuses the value.
 */
#ifndef RYZEN_MSRIO_HPP
#define RYZEN_MSRIO_HPP

#include <stdint.h>
#include <i386/proc_reg.h>
#include "KernelDecls.hpp"

static inline bool
readMsr(uint32_t msr, uint64_t *value)
{
    uint32_t lo = 0, hi = 0;
    if (rdmsr_carefully(msr, &lo, &hi) != 0) {
        return false;
    }
    *value = ((uint64_t)hi << 32) | lo;
    return true;
}

static inline bool
writeMsr(uint32_t msr, uint64_t value)
{
    return wrmsr_carefully(msr, (uint32_t)value, (uint32_t)(value >> 32)) == 0;
}

/*
 * Runs fn on every CPU with interrupts off. Serialised with a lock: the sampler
 * (workloop thread) and the actuator (user client / watchdog threads) both
 * rendezvous, and nothing here is known to make concurrent rendezvous safe.
 * fn must not take locks or call anything that can sleep.
 */
void ryzenRendezvous(void (*fn)(void *));

#endif /* RYZEN_MSRIO_HPP */
