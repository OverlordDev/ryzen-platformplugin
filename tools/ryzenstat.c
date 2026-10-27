/*
 * ryzenstat - read the RyzenPlatform.kext shadow data and print it.
 *
 *   ryzenstat            one sample
 *   ryzenstat -w 5       five samples, one per second
 *
 * No root needed: everything comes from the IORegistry.
 *
 * C0% and effMHz are deltas between two kext samples, so the first sample of a
 * run shows "-". Use -w 2 or more to see them. C0% tells you whether idle cores
 * really sleep (high at idle = they do not, and no frequency policy will fix
 * the heat); effMHz is the clock the core actually ran at while awake.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

static io_service_t gService;

static CFTypeRef
prop(const char *key)
{
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key,
                                              kCFStringEncodingUTF8);
    if (!k) {
        return NULL;
    }
    CFTypeRef ref = IORegistryEntryCreateCFProperty(gService, k,
                                                    kCFAllocatorDefault, 0);
    CFRelease(k);
    return ref;
}

static int
getU32(const char *key, uint32_t *out)
{
    CFTypeRef ref = prop(key);
    if (!ref) {
        return 0;
    }
    int ok = 0;
    if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
        long long v = 0;
        if (CFNumberGetValue(ref, kCFNumberLongLongType, &v)) {
            *out = (uint32_t)v;
            ok = 1;
        }
    }
    CFRelease(ref);
    return ok;
}

static int
getS32(const char *key, int32_t *out)
{
    CFTypeRef ref = prop(key);
    if (!ref) {
        return 0;
    }
    int ok = 0;
    if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
        long long v = 0;
        if (CFNumberGetValue(ref, kCFNumberLongLongType, &v)) {
            *out = (int32_t)v;
            ok = 1;
        }
    }
    CFRelease(ref);
    return ok;
}

static int
getBool(const char *key, int *out)
{
    CFTypeRef ref = prop(key);
    if (!ref) {
        return 0;
    }
    *out = (ref == kCFBooleanTrue);
    CFRelease(ref);
    return 1;
}

/* Returns a malloc'd array of n uint64. */
static uint64_t *
getArray(const char *key, uint32_t *count)
{
    CFTypeRef ref = prop(key);
    if (!ref || CFGetTypeID(ref) != CFDataGetTypeID()) {
        if (ref) CFRelease(ref);
        return NULL;
    }
    CFIndex len = CFDataGetLength(ref);
    const UInt8 *bytes = CFDataGetBytePtr(ref);
    uint32_t n = (uint32_t)(len / sizeof(uint64_t));
    uint64_t *out = calloc(n ? n : 1, sizeof(uint64_t));
    if (out) {
        memcpy(out, bytes, n * sizeof(uint64_t));
        *count = n;
    }
    CFRelease(ref);
    return out;
}

#define U64_NONE 0xFFFFFFFFFFFFFFFFull

static const char *
yesno(int v)
{
    return v ? "yes" : "no";
}

/* TSC (= MPERF reference) rate in MHz, or 0 if macOS will not say. On a
 * Hackintosh this value can be wrong; treat effMHz as a cross-check. */
static double
tscMHz(void)
{
    uint64_t hz = 0;
    size_t len = sizeof(hz);
    if (sysctlbyname("machdep.tsc.frequency", &hz, &len, NULL, 0) == 0 && hz) {
        return hz / 1e6;
    }
    hz = 0;
    len = sizeof(hz);
    if (sysctlbyname("hw.tscfrequency", &hz, &len, NULL, 0) == 0 && hz) {
        return hz / 1e6;
    }
    return 0.0;
}

/* C0 residency (%) and effective MHz from two samples of one CPU. Returns 0
 * when there is nothing to compute (no previous sample, same kext sample read
 * twice, counters missing or went backwards). */
static int
rates(const uint64_t *ap, const uint64_t *mp, const uint64_t *ts,
      const uint64_t *pa, const uint64_t *pm, const uint64_t *pt,
      uint32_t i, double tsc, double *c0, double *eff)
{
    if (!ap || !mp || !ts || !pa || !pm || !pt) {
        return 0;
    }
    if (ap[i] == U64_NONE || mp[i] == U64_NONE || pa[i] == U64_NONE || pm[i] == U64_NONE) {
        return 0;
    }
    if (ts[i] <= pt[i] || mp[i] < pm[i] || ap[i] < pa[i]) {
        return 0;
    }
    double dt = (double)(ts[i] - pt[i]);
    double dm = (double)(mp[i] - pm[i]);
    double da = (double)(ap[i] - pa[i]);
    *c0  = 100.0 * dm / dt;
    *eff = (dm > 0.0 && tsc > 0.0) ? tsc * da / dm : 0.0;
    return 1;
}

static void
fmtRates(char *buf, size_t n, int have, double c0, double eff)
{
    if (!have) {
        snprintf(buf, n, "    -       -");
    } else if (eff > 0.0) {
        snprintf(buf, n, "%5.1f %7.0f", c0, eff);
    } else {
        snprintf(buf, n, "%5.1f       -", c0);
    }
}

int
main(int argc, char **argv)
{
    int watch = 1;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--watch") == 0) && i + 1 < argc) {
            watch = atoi(argv[++i]);
            if (watch < 1) watch = 1;
        }
    }

    gService = IOServiceGetMatchingService(kIOMainPortDefault,
                                           IOServiceMatching("com_illia_RyzenPlatform"));
    if (!gService) {
        fprintf(stderr, "RyzenPlatform.kext is not loaded (no matching IOService)\n");
        return 1;
    }

    int cppcSupported = 0;
    getBool("cppc.supported", &cppcSupported);
    double tsc = tscMHz();
    uint64_t *pAp = NULL, *pMp = NULL, *pTs = NULL;

    uint32_t nCpu = 0;
    if (!getU32("cpu.count", &nCpu) || nCpu == 0 || nCpu > 64) {
        fprintf(stderr, "kext did not publish a sane cpu.count\n");
        return 1;
    }

    printf("RyzenPlatform shadow reader\n");
    printf("  CPUs: %u    CPPC advertised by CPUID: %s\n", nCpu, yesno(cppcSupported));
    printf("  No MSR is written. Values come from the SMU via rdmsr.\n");
    printf("  TSC: %s\n\n", tsc > 0.0 ? "known" : "unknown, effMHz unavailable");

    for (int iter = 0; iter < watch; iter++) {
        if (iter > 0) {
            /* Slightly over the kext's 1 s period so every read is a new sample. */
            usleep(1100 * 1000);
        }

        int tempValid = 0;
        int32_t tctl = 0;
        int haveTemp = getBool("temp.valid", &tempValid) && tempValid &&
                       getS32("temp.tctl", &tctl);
        int range49 = 0;
        getBool("temp.range49", &range49);
        uint32_t smnRetries = 0;
        getU32("smn.retries", &smnRetries);
        uint32_t enabled = 0;
        getU32("cppc.enabled", &enabled);

        uint32_t n1 = 0, n2 = 0, n3 = 0, n4 = 0, n5 = 0, n6 = 0, n7 = 0;
        uint64_t *cap1 = getArray("cppc.cap1", &n1);
        uint64_t *enab = getArray("cppc.enable", &n2);
        uint64_t *cap2 = getArray("cppc.cap2", &n3);
        uint64_t *req  = getArray("cppc.req", &n4);
        uint64_t *stat = getArray("cppc.status", &n5);
        uint64_t *pst  = getArray("pstate", &n6);
        uint64_t *pdef = getArray("pstate.def", &n7);
        uint32_t n8 = 0, n9 = 0, n10 = 0;
        uint64_t *aperf = getArray("aperf", &n8);
        uint64_t *mperf = getArray("mperf", &n9);
        uint64_t *tscs  = getArray("tsc", &n10);

        if (!cap1 || !stat || !pst) {
            fprintf(stderr, "kext properties are incomplete\n");
            return 1;
        }

        if (haveTemp) {
            printf("t=%02d  Tctl=%.3f C%s   CPPC enabled on %u/%u cores\n",
                   iter, tctl / 1000.0, range49 ? " (range bit set, -49 C applied)" : "",
                   enabled, nCpu);
        } else {
            printf("t=%02d  Tctl=n/a   CPPC enabled on %u/%u cores\n",
                   iter, enabled, nCpu);
        }
        if (smnRetries) {
            printf("  warning: %u SMN index/data interleaves caught - another kext uses the same pair\n",
                   smnRetries);
        }

        if (cppcSupported) {
            printf("  cpu  cap1:lo/nl/nom/hi   req:min/max/des/epp   en  status  P  FID DID   MHz    C0%% effMHz\n");
        } else {
            printf("  cpu  (CPPC not advertised)                        P  FID DID   MHz    C0%% effMHz\n");
        }

        for (uint32_t i = 0; i < nCpu; i++) {
            uint64_t c1 = cap1[i];
            uint64_t rq = req ? req[i] : 0;
            uint64_t en = enab ? enab[i] : 0;
            uint64_t st = stat[i];
            uint64_t ps = pst[i];
            uint64_t pd = pdef ? pdef[i] : U64_NONE;

            uint32_t fid = (uint32_t)(pd & 0xFF);
            uint32_t did = (uint32_t)((pd >> 8) & 0x3F);
            uint32_t vid = (uint32_t)((pd >> 14) & 0x1FF);
            double mhz = (pd == U64_NONE || did == 0) ? 0.0 : 200.0 * fid / did;

            double c0 = 0.0, eff = 0.0;
            int haveRates = (n8 == nCpu && n9 == nCpu && n10 == nCpu &&
                             rates(aperf, mperf, tscs, pAp, pMp, pTs, i, tsc, &c0, &eff));
            char rt[32];
            fmtRates(rt, sizeof(rt), haveRates, c0, eff);

            if (cppcSupported) {
                printf("  %3u   %3u/%3u/%3u/%3u      %3u/%3u/%3u/%3u   %3u  %s  %2u  %3u %3u  %7.0f  %s\n",
                       i,
                       (unsigned)(c1 & 0xFF), (unsigned)((c1 >> 8) & 0xFF),
                       (unsigned)((c1 >> 16) & 0xFF), (unsigned)((c1 >> 24) & 0xFF),
                       (unsigned)(rq & 0xFF), (unsigned)((rq >> 8) & 0xFF),
                       (unsigned)((rq >> 16) & 0xFF), (unsigned)((rq >> 24) & 0xFF),
                       (unsigned)(en & 0xFF),
                       (st & 1) ? "on " : "off",
                       (unsigned)(ps == U64_NONE ? 0 : (ps & 7)),
                       fid, did, mhz, rt);
            } else {
                printf("  %3u                                                        %2u  %3u %3u  %7.0f  %s\n",
                       i, (unsigned)(ps == U64_NONE ? 0 : (ps & 7)), fid, did, mhz, rt);
            }
            (void)vid;
        }
        printf("\n");

        free(cap1); free(enab); free(cap2); free(req);
        free(stat); free(pst); free(pdef);

        /* This sample becomes the baseline for the next one. */
        free(pAp); free(pMp); free(pTs);
        pAp = aperf; pMp = mperf; pTs = tscs;
    }
    free(pAp); free(pMp); free(pTs);

    IOObjectRelease(gService);
    return 0;
}
