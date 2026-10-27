#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int rdmsr_carefully(uint32_t, uint32_t*, uint32_t*);
#ifdef __cplusplus
}
#endif
