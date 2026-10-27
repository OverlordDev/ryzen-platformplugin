/*
 * ryzenstat - read the RyzenPlatform.kext shadow data and print it.
 *
 *   ryzenstat            one sample
 *   ryzenstat -w 5       five samples, one per second
 *
 * No root needed: everything comes from the IORegistry.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

    uint32_t nCpu = 0;
    if (!getU32("cpu.count", &nCpu) || nCpu == 0 || nCpu > 64) {
        fprintf(stderr, "kext did not publish a sane cpu.count\n");
        return 1;
    }

    printf("RyzenPlatform shadow reader\n");
    printf("  CPUs: %u    CPPC advertised by CPUID: %s\n", nCpu, yesno(cppcSupported));
    printf("  No MSR is written. Values come from the SMU via rdmsr.\n\n");

    for (int iter = 0; iter < watch; iter++) {
        if (iter > 0) {
            sleep(1);
        }

        uint32_t tctl = 0;
        int haveTemp = getU32("temp.tctl", &tctl) && tctl != 0xFFFFFFFFu;
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

        if (!cap1 || !stat || !pst) {
            fprintf(stderr, "kext properties are incomplete\n");
            return 1;
        }

        printf("t=%02d  Tctl=%s%.3f C   CPPC enabled on %u/%u cores\n",
               iter,
               haveTemp ? "" : "n/a ", haveTemp ? tctl / 1000.0 : 0.0,
               enabled, nCpu);

        if (cppcSupported) {
            printf("  cpu  cap1:lo/nl/nom/hi   req:min/max/des/epp   en  status  P  FID DID   MHz\n");
        } else {
            printf("  cpu  (CPPC not advertised)                        P  FID DID   MHz\n");
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

            if (cppcSupported) {
                printf("  %3u   %3u/%3u/%3u/%3u      %3u/%3u/%3u/%3u   %3u  %s  %2u  %3u %3u  %7.0f\n",
                       i,
                       (unsigned)(c1 & 0xFF), (unsigned)((c1 >> 8) & 0xFF),
                       (unsigned)((c1 >> 16) & 0xFF), (unsigned)((c1 >> 24) & 0xFF),
                       (unsigned)(rq & 0xFF), (unsigned)((rq >> 8) & 0xFF),
                       (unsigned)((rq >> 16) & 0xFF), (unsigned)((rq >> 24) & 0xFF),
                       (unsigned)(en & 0xFF),
                       (st & 1) ? "on " : "off",
                       (unsigned)(ps == U64_NONE ? 0 : (ps & 7)),
                       fid, did, mhz);
            } else {
                printf("  %3u                                                        %2u  %3u %3u  %7.0f\n",
                       i, (unsigned)(ps == U64_NONE ? 0 : (ps & 7)), fid, did, mhz);
            }
            (void)vid;
        }
        printf("\n");

        free(cap1); free(enab); free(cap2); free(req);
        free(stat); free(pst); free(pdef);
    }

    IOObjectRelease(gService);
    return 0;
}
