// Host-side mock of just enough kernel for Actuator.cpp.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <map>
#include "KernelDecls.hpp"
#include "RyzenPlatform.hpp"
#include "ZenMsr.h"

struct IOLock { int d; };
IOLock *IOLockAlloc(){ return new IOLock{0}; }
void IOLockFree(IOLock*l){ delete l; }
void IOLockLock(IOLock*){} void IOLockUnlock(IOLock*){}
void IOLog(const char*f,...){ if(getenv("VERBOSE")){ va_list a; va_start(a,f); vprintf(f,a); va_end(a);} }
size_t strlcpy(char*d,const char*s,size_t n){ size_t l=strlen(s); if(n){ size_t c=l>=n?n-1:l; memcpy(d,s,c); d[c]=0;} return l; }

// ---- fake CPU/MSR world
int g_ncpu=4, g_cur=0;
uint64_t g_msr[8][0x100]; // index by low byte of the MSR (0xB0..0xB4)
static int idx(uint32_t msr){ return msr & 0xFF; }
bool g_failWriteCpu=false; int g_failCpu=-1;      // wrmsr refused
bool g_corruptReadbackCpu=false; int g_corruptCpu=-1; // writes, but reads back different
int g_writes=0;
int cpu_number(void){ return g_cur; }
void mp_rendezvous_no_intrs(void(*fn)(void*), void*a){ for(int c=0;c<g_ncpu;c++){ g_cur=c; fn(a);} g_cur=0; }
extern "C" int rdmsr_carefully(uint32_t msr,uint32_t*lo,uint32_t*hi){ uint64_t v=g_msr[g_cur][idx(msr)]; *lo=(uint32_t)v; *hi=(uint32_t)(v>>32); return 0; }
int wrmsr_carefully(uint32_t msr,uint32_t lo,uint32_t hi){
  if(g_failWriteCpu && g_cur==g_failCpu) return 1;
  uint64_t v=((uint64_t)hi<<32)|lo;
  if(g_corruptReadbackCpu && g_cur==g_corruptCpu) v ^= 0x1;
  g_msr[g_cur][idx(msr)]=v; g_writes++; return 0; }
uint64_t g_now=1000000000ull;
void clock_get_uptime(uint64_t*r){ *r=g_now; }
void absolutetime_to_nanoseconds(uint64_t a,uint64_t*r){ *r=a; }

// ---- sampler/temperature mocks
bool g_tempOk=true; int32_t g_temp=45000;
bool ryzenReadTctl(int32_t*m){ if(!g_tempOk) return false; *m=g_temp; return true; }
uint32_t ryzenSmnRetries(void){ return 0; }
bool ryzenGetSample(uint32_t cpu, RyzenCpuSample*o){ if((int)cpu>=g_ncpu) return false;
  o->cap1=g_msr[cpu][idx(MSR_AMD_CPPC_CAP1)]; o->enable=g_msr[cpu][idx(MSR_AMD_CPPC_ENABLE)]; o->cap2=0;
  o->req=g_msr[cpu][idx(MSR_AMD_CPPC_REQ)]; o->status=g_msr[cpu][idx(MSR_AMD_CPPC_STATUS)];
  o->pstate=0; o->pstateDef=(16ull)|(1ull<<8)|0; return true; } // fid=16 did=1 -> 3200 MHz
