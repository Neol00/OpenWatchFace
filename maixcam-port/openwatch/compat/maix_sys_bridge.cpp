/* ============================================================================
 *  maix_sys_bridge.cpp — real device/system info for the About + Power screens.
 *
 *  Reads the standard Linux /proc + /sys interfaces (reliable, well-defined) for
 *  memory / CPU freq / temperature / usage / disk, and maix::sys for the OS
 *  version string. Exposed as extern "C" (owf_maix_sys_*) so the firmware screens
 *  can show real values instead of the ESP-placeholder stubs.
 *  MaixCDK-only TU (no Arduino.h).
 * ========================================================================== */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <sys/statvfs.h>
#include "maix_basic.hpp"   // maix::sys
#include "owf_maix_hooks.h"

using namespace maix;

/* read the first integer value of a /proc/meminfo line like "MemTotal: 123 kB" */
static long meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[128]; long val = 0; size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0) { sscanf(line + klen, " %ld", &val); break; }
    }
    fclose(f);
    return val;
}
long owf_maix_mem_total_kb(void) { return meminfo_kb("MemTotal:"); }
long owf_maix_mem_avail_kb(void) { long a = meminfo_kb("MemAvailable:"); return a ? a : meminfo_kb("MemFree:"); }

static long read_long_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long v = -1; if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v;
}

int owf_maix_cpu_mhz(void)
{
    long khz = read_long_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    if (khz <= 0) khz = read_long_file("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    if (khz > 0) return (int)(khz / 1000);
    return 1000;   // SG2002 C906 runs a fixed ~1 GHz; cpufreq sysfs isn't exposed
}

/* Physical RAM carved out from Linux (CVITEK ION/media heaps for VPU/ISP/NPU):
 * the /proc/iomem "System RAM" span minus what Linux actually manages (MemTotal). */
long owf_maix_mem_reserved_kb(void)
{
    FILE *f = fopen("/proc/iomem", "r");
    if (!f) return 0;
    char line[256];
    unsigned long long phys = 0, a, b;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "System RAM") && sscanf(line, "%llx-%llx", &a, &b) == 2)
            phys += (b - a + 1);
    }
    fclose(f);
    long reserved = (long)(phys / 1024) - meminfo_kb("MemTotal:");
    return reserved > 0 ? reserved : 0;
}

int owf_maix_cpu_temp_c(void)
{
    long milli = read_long_file("/sys/class/thermal/thermal_zone0/temp");
    return milli > 0 ? (int)((milli + 500) / 1000) : -273;   // -273 = unknown
}

/* CPU usage % since the previous call (delta of /proc/stat aggregate line). */
int owf_maix_cpu_usage_pct(void)
{
    static long prev_total = 0, prev_idle = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;
    long u = 0, n = 0, s = 0, idle = 0, iow = 0, irq = 0, sirq = 0;
    if (fscanf(f, "cpu %ld %ld %ld %ld %ld %ld %ld", &u, &n, &s, &idle, &iow, &irq, &sirq) < 4) { fclose(f); return 0; }
    fclose(f);
    long total = u + n + s + idle + iow + irq + sirq;
    long dt = total - prev_total, di = idle - prev_idle;
    prev_total = total; prev_idle = idle;
    if (dt <= 0) return 0;
    int pct = (int)(100 * (dt - di) / dt);
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

long owf_maix_disk_total_kb(void) { struct statvfs s; return statvfs("/", &s) == 0 ? (long)((uint64_t)s.f_blocks * s.f_frsize / 1024) : 0; }
long owf_maix_disk_free_kb(void)  { struct statvfs s; return statvfs("/", &s) == 0 ? (long)((uint64_t)s.f_bavail * s.f_frsize / 1024) : 0; }

void owf_maix_os_version(char *buf, int cap)
{
    if (!buf || cap <= 0) return;
    std::string v;
    try { v = sys::os_version(); } catch (...) {}
    strncpy(buf, v.c_str(), cap - 1);
    buf[cap - 1] = 0;
}
