#include <IOKit/IOService.h>
#include <stdio.h>
#include <string.h>
#include "Actuator.hpp"
#include "ZenMsr.h"
extern int g_ncpu, g_writes, g_failCpu, g_corruptCpu; extern bool g_failWriteCpu, g_corruptReadbackCpu, g_tempOk;
extern uint64_t g_msr[8][0x100]; extern uint64_t g_now; extern int32_t g_temp;
static int fails=0;
#define CHECK(c,msg) do{ if(!(c)){ printf("  FAIL: %s  (%s:%d)\n",msg,__FILE__,__LINE__); fails++; } else printf("  ok: %s\n",msg);}while(0)
static uint64_t FW_REQ = (150ull) | (20ull<<8) | (0ull<<16) | (128ull<<24);   // firmware: max150 min20 des0 epp128
static uint64_t REQ(int c){ return g_msr[c][0xB3]; }
static void resetWorld(){
  memset(g_msr,0,sizeof g_msr); g_ncpu=4; g_writes=0; g_failWriteCpu=g_corruptReadbackCpu=false; g_tempOk=true; g_temp=45000;
  for(int c=0;c<4;c++){ g_msr[c][0xB0]=20ull|(40ull<<8)|(100ull<<16)|(150ull<<24); g_msr[c][0xB1]=1; g_msr[c][0xB4]=1; g_msr[c][0xB3]=FW_REQ; }
}
static ryzen_apply_t mkApply(uint32_t core,uint32_t max,uint32_t min,uint32_t des,uint32_t epp,uint32_t ttl=0){
  ryzen_apply_t a; memset(&a,0,sizeof a); a.abi_version=RYZEN_PLATFORM_ABI_VERSION; a.struct_size=sizeof a; a.count=1;
  a.req[0]={core,des,max,min,epp,ttl}; return a; }
static void beat(){ ryzen_heartbeat_t h; memset(&h,0,sizeof h); h.abi_version=RYZEN_PLATFORM_ABI_VERSION; h.struct_size=sizeof h; actHeartbeat(&h); }
static void begin(bool writeOn){ resetWorld(); actInit(true,writeOn); actSetCpuCount(4); }
static void end(){ actShutdown(); }
// Realistic cadence: the daemon beats and the kext ticks every 50 ms.
static void run(int ms){ for(int t=0;t<ms;t+=50){ g_now+=50e6; beat(); actTick(); } }

int main(){
  printf("[1] writes are off by default\n");
  begin(false); { auto a=mkApply(0,150,20,130,0); CHECK(actApply(&a)==RYZEN_E_DISABLED,"apply refused"); CHECK(g_writes==0,"no MSR written"); } end();

  printf("[2] clamp, per-CPU isolation, upper bits preserved, exact restore\n");
  begin(true); g_msr[0][0xB3] = FW_REQ | (0xABull<<32);
  { auto a=mkApply(0,200,0,250,0); CHECK(actApply(&a)==RYZEN_OK,"apply ok");
    uint64_t r=REQ(0); CHECK((r&0xFF)==150,"max clamped to highest 150"); CHECK(((r>>8)&0xFF)==20,"min 0 -> lowest 20");
    CHECK(((r>>16)&0xFF)==150,"desired 250 clamped into [min,max] = 150"); CHECK(((r>>24)&0xFF)==0,"epp 0");
    CHECK((r>>32)==0xAB,"reserved upper half preserved");
    CHECK(REQ(1)==FW_REQ && REQ(2)==FW_REQ,"other CPUs untouched");
    CHECK(actResetAuto("t",true)==RYZEN_OK,"reset ok"); CHECK(REQ(0)==(FW_REQ|(0xABull<<32)),"firmware value restored bit-exact"); } end();

  printf("[3] bad CPU index rejects the whole request, writes nothing\n");
  begin(true); { ryzen_apply_t a=mkApply(0,150,20,100,0); a.count=2; a.req[1]={9,100,150,20,0,0};
    CHECK(actApply(&a)==RYZEN_E_RANGE,"E_RANGE"); CHECK(g_writes==0,"nothing written, even the valid entry"); } end();

  printf("[4] CPPC_ENABLE clear -> refuse, never enable it ourselves\n");
  begin(true); g_msr[2][0xB1]=0; { auto a=mkApply(0,150,20,100,0); CHECK(actApply(&a)==RYZEN_E_UNSUPPORTED,"unsupported"); CHECK(g_msr[2][0xB1]==0 && g_writes==0,"ENABLE untouched, no writes"); } end();

  printf("[5] watchdog: silence for >150 ms restores firmware\n");
  begin(true); { auto a=mkApply(0,150,20,100,0); actApply(&a); CHECK(REQ(0)!=FW_REQ,"override live");
    g_now+=100e6; actTick(); CHECK(REQ(0)!=FW_REQ,"still live at 100 ms"); beat(); g_now+=100e6; actTick(); CHECK(REQ(0)!=FW_REQ,"heartbeat keeps it alive");
    g_now+=160e6; actTick(); CHECK(REQ(0)==FW_REQ,"restored after 160 ms of silence"); CHECK(!actNeedsTick(),"no more ticks needed");
    ryzen_status_t s; actStatus(&s); CHECK(s.watchdog_resets==1,"watchdog counted"); } end();

  printf("[6] TTL reverts only the expired CPU\n");
  begin(true); { ryzen_apply_t a=mkApply(0,150,20,100,0,30); a.count=2; a.req[1]={1,100,150,20,0,0}; actApply(&a);
    beat(); g_now+=40e6; actTick(); CHECK(REQ(0)==FW_REQ,"cpu0 (ttl 30 ms) reverted"); CHECK(REQ(1)!=FW_REQ,"cpu1 (no ttl) kept"); } end();

  printf("[7] thermal guard: 90 cap, 98 lowest, 100 restore+lockout\n");
  begin(true); { auto a=mkApply(0,150,20,140,0); actApply(&a);
    g_temp=91000; run(300); uint64_t r=REQ(0);
    CHECK((r&0xFF)==100 && ((r>>16)&0xFF)<=100,"capped at nominal 100"); CHECK(((r>>24)&0xFF)==255,"epp 255 under cap");
    g_temp=99000; run(300); r=REQ(0);
    CHECK((r&0xFF)==20 && ((r>>8)&0xFF)==20 && ((r>>16)&0xFF)==20,"everything at lowest at 99 C");
    g_temp=88000; run(300); r=REQ(0); CHECK((r&0xFF)==100,"88 C holds the cap (hysteresis)");
    g_temp=80000; run(300); r=REQ(0); CHECK((r&0xFF)==150 && ((r>>16)&0xFF)==140,"80 C releases to the original request");
    g_temp=101000; run(300); CHECK(REQ(0)==FW_REQ,"100 C restores firmware"); CHECK(!actNeedsTick(),"override ended");
    auto b=mkApply(0,150,20,140,0); CHECK(actApply(&b)==RYZEN_E_LOCKOUT,"apply refused while locked out");
    CHECK(actResetAuto("t",true)==RYZEN_E_LOCKOUT,"reset cannot clear lockout while hot"); g_temp=60000;
    CHECK(actResetAuto("t",true)==RYZEN_OK,"reset clears lockout once cool"); CHECK(actApply(&b)==RYZEN_OK,"apply works again"); } end();

  printf("[8] no sensor is NOT 'cool': cap at nominal\n");
  begin(true); { auto a=mkApply(0,150,20,140,0); actApply(&a); g_tempOk=false; run(300);
    CHECK((REQ(0)&0xFF)==100,"capped at nominal without a temperature"); ryzen_status_t s; actStatus(&s); CHECK(s.guard_level==1 && s.tctl_millic==RYZEN_TEMP_UNKNOWN,"status shows guard + unknown temp"); } end();

  printf("[9] wrmsr refused on one CPU -> everything restored, lockout\n");
  begin(true); g_failWriteCpu=true; g_failCpu=2; { ryzen_apply_t a=mkApply(0,150,20,100,0); a.count=3; a.req[1]={1,100,150,20,0,0}; a.req[2]={2,100,150,20,0,0};
    CHECK(actApply(&a)==RYZEN_E_READBACK,"E_READBACK"); g_failWriteCpu=false;
    CHECK(REQ(0)==FW_REQ && REQ(1)==FW_REQ && REQ(2)==FW_REQ,"all CPUs back at firmware value");
    ryzen_status_t s; actStatus(&s); CHECK((s.flags&RYZEN_FLAG_LOCKOUT)&&s.readback_failures==1,"lockout latched, failure counted"); } end();

  printf("[10] read-back mismatch is caught too\n");
  begin(true); g_corruptReadbackCpu=true; g_corruptCpu=1; { ryzen_apply_t a=mkApply(0,150,20,100,0); a.count=2; a.req[1]={1,100,150,20,0,0};
    CHECK(actApply(&a)==RYZEN_E_READBACK,"E_READBACK on mismatch"); } end();

  printf("[11] sleep: restore, refuse applies, wake re-captures\n");
  begin(true); { auto a=mkApply(0,150,20,100,0); actApply(&a); actPrepareForSleep(); CHECK(REQ(0)==FW_REQ,"restored before sleep");
    CHECK(actApply(&a)!=RYZEN_OK,"apply refused while sleeping"); actWake();
    g_msr[0][0xB3]=(140ull)|(30ull<<8)|(0ull<<16)|(64ull<<24); // firmware changed it across sleep
    CHECK(actApply(&a)==RYZEN_OK,"apply works after wake"); actResetAuto("t",true);
    CHECK(REQ(0)==((140ull)|(30ull<<8)|(64ull<<24)),"restores the NEW firmware value, not a stale one"); } end();

  printf("[12] ABI mismatch\n");
  begin(true); { auto a=mkApply(0,150,20,100,0); a.abi_version=1; CHECK(actApply(&a)==RYZEN_E_ABI,"old ABI refused"); a.abi_version=RYZEN_PLATFORM_ABI_VERSION; a.struct_size=4; CHECK(actApply(&a)==RYZEN_E_ABI,"bad struct_size refused"); } end();

  printf("\n%s (%d failure%s)\n", fails?"FAILED":"ALL PASSED", fails, fails==1?"":"s");
  return fails?1:0;
}
