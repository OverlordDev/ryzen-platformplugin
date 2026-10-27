/*
 * KernelDecls.hpp - kernel symbols that are exported but live outside the
 * public headers. Same set that RyzenHV already links against, so these are
 * known to resolve on this machine.
 */
#ifndef RYZEN_PLATFORM_KERNELDECLS_HPP
#define RYZEN_PLATFORM_KERNELDECLS_HPP

#include <stdint.h>

extern "C" {
/* Runs `action` on every CPU with interrupts off. */
void mp_rendezvous_no_intrs(void (*action)(void *), void *arg);
/* Index of the CPU we are running on right now. */
int  cpu_number(void);
}

#endif /* RYZEN_PLATFORM_KERNELDECLS_HPP */
