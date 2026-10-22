/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Files, directories, memory mapping, time, signals, polling, sockets.
 *
 * Everything here crosses a boundary where Linux and Darwin disagree about a number or
 * a layout: open flags, the shape of struct stat and struct dirent, mmap flags, clock
 * ids, signal numbers, errno values. Each wrapper translates in and out and publishes
 * errno in the guest's own numbering.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-bionic.h"
const char *tl_ld_main_apk(void);
#include "husk-tl-internal.h"
#include "husk-tl-codewrite.h"
#include "husk-tl-xmem.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <TargetConditionals.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <limits.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <xlocale.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <mach/mach.h>
#include <time.h>
#include <search.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <libkern/OSCacheControl.h>
#include <unistd.h>
#include <time.h>
#include <utime.h>

/* ------------------------------------------------- app-remembered RW-exec memory
 *
 * A guest's OWN writable mapping (b_mmap's RW-only branch, below) is handed the WRITABLE view, so a
 * store into it is a native store and does not fault. But that branch exists for a JIT (LuaJIT: it maps
 * near its code, writes machine code, then mprotects it executable). A JIT handed the writable view that
 * then calls mprotect(PROT_EXEC) reaches tl_xmem_allow_exec, which cannot grant execute to that alias and
 * refuses with EACCES (b_mprotect, below) -- honest, but a JIT that does not check errno would call into
 * non-executable memory and die.
 *
 * So the fact is REMEMBERED, in the app's own TranslationLayer/<uuid>/... folder: on its next launch that
 * app is handed the executable view for its writable mappings again (its old behaviour), while every other
 * title keeps the native stores. The file is `<data dir>/.husk-jit-writable` and holds the package and
 * version it was recorded for, so a different app or a different version never inherits the note.
 * DELETE THAT FILE to forget it. See data-out-of-the-executable-view.md.
 */
const char *tl_hle_package(void);
int tl_hle_version_code(void);
static int  g_rw_mem_loaded;            /* 0 until the note is read for the app now running */
static bool g_rw_app_needs_exec;        /* this app writes code into writable memory (a JIT) */
static void rw_exec_memory_load(void);
static void rw_exec_memory_record(uintptr_t addr, size_t len);

/* ------------------------------------------------------------ path mapping */

/*
 * Android paths the guest expects, mapped to somewhere that exists. The data
 * directory is wherever the host set aside for this app; /proc files that apps read
 * to learn about the device are synthesised into unlinked temporary files.
 */
static char g_data_dir[512];
/* New files a game never finished, because Husk was ended while it wrote them (tl_atomic_open): the old files beside them are whole. */
static void drop_unfinished(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        char p[1100];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        size_t n = strlen(e->d_name);
        if (e->d_type == DT_DIR) { if (depth < 12) drop_unfinished(p, depth + 1); }
        else if (n > 9 && !strcmp(e->d_name + n - 9, ".husk-new") && unlink(p) == 0) tl_log_line("file: dropped %s, a save never finished", p);
    }
    closedir(d);
}
void tl_set_data_dir(const char *dir)
{
    snprintf(g_data_dir, sizeof(g_data_dir), "%s", dir ? dir : "");
    g_rw_mem_loaded = 0;                    /* a new app: read ITS RW-exec note (lazily, on first use) */
    g_rw_app_needs_exec = false;
    if (g_data_dir[0]) drop_unfinished(g_data_dir, 0);
}
const char *tl_data_dir(void) { return g_data_dir[0] ? g_data_dir : "/tmp"; }

/* ------------------------------------------------------------ load-path accounting
 *
 * See husk-tl-internal.h. Accumulated in relaxed atomics by the layer's own inflate / copy
 * / map sites, printed once at session close (tl_io_reset_for_new_app). This is the answer
 * to "which phase is the minute": a number, not a guess. Nothing here runs per frame, and
 * nothing here allocates or locks. */
static volatile unsigned long long g_load_ns[TL_LOAD_N];
static volatile unsigned long long g_load_bytes[TL_LOAD_N];
static volatile unsigned long g_load_calls[TL_LOAD_N];
static const char *const k_load_name[TL_LOAD_N] = { "inflate", "copy", "hash", "map", "asset" };

unsigned long long tl_load_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}
void tl_load_add(int phase, unsigned long long ns, unsigned long long bytes)
{
    if (phase < 0 || phase >= TL_LOAD_N) return;
    __atomic_fetch_add(&g_load_ns[phase], ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_load_bytes[phase], bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_load_calls[phase], 1ul, __ATOMIC_RELAXED);
}
static void ss_put_load(char *line, int *w, size_t cap, const char *fmt, ...)
{
    if (*w < 0 || (size_t)*w >= cap) return;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line + *w, cap - (size_t)*w, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - (size_t)*w) *w = (int)cap - 1; else *w += n;
}
void tl_load_summary(const char *tag)
{
    char line[640];
    int w = 0;
    line[0] = 0;
    ss_put_load(line, &w, sizeof(line), "load[%s]:", tag ? tag : "?");
    for (int i = 0; i < TL_LOAD_N; i++) {
        unsigned long calls = __atomic_load_n(&g_load_calls[i], __ATOMIC_RELAXED);
        if (!calls) continue;
        unsigned long long ns = __atomic_load_n(&g_load_ns[i], __ATOMIC_RELAXED);
        unsigned long long bytes = __atomic_load_n(&g_load_bytes[i], __ATOMIC_RELAXED);
        ss_put_load(line, &w, sizeof(line), " %s=%.3fs/%lu call(s)/%.1f MiB",
                    k_load_name[i], (double)ns / 1e9, calls, (double)bytes / 1048576.0);
    }
    ss_put_load(line, &w, sizeof(line), " -- inflate is the CPU a cache removes; copy is a later launch reading the cache");
    tl_log_line("%s", line);
}

/* -------------------------------------------------------- the inflated-bytes cache
 *
 * An APK entry's bytes, preferring an on-disk copy of the INFLATED result (see
 * husk-tl-internal.h for why). The identity is the whole point: the file name is a 64-bit
 * FNV-1a over the APK's path, its size and mtime, the entry's name, and both of its sizes,
 * so any change -- an updated APK, a rebuilt entry, a re-zipped one -- yields a different
 * name and a stale copy is never even looked at. The file is written under a .part name
 * and renamed into place, so a launch that dies mid-write can never leave a torn file; and
 * a read-back shorter than the size the directory promised is discarded and the entry
 * inflated afresh. A corrupted cache is worse than a slow load, so the cache is trusted
 * only when it is exactly right. */
static unsigned long long load_fnv(const char *s, unsigned long long h)
{
    if (!s) return h;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) { h ^= *p; h *= 1099511628211ull; }
    return h;
}
static uint64_t load_entry_identity(const char *apk, const char *entry, uint64_t usize, uint64_t csize)
{
    uint64_t h = 1469598103934665603ull;
    h = load_fnv(apk, h);   h ^= 0x1full; h *= 1099511628211ull;
    h = load_fnv(entry, h); h ^= 0x1full; h *= 1099511628211ull;
    uint64_t extra = 0;
    struct stat st;
    if (apk && stat(apk, &st) == 0) extra = (uint64_t)st.st_size ^ ((uint64_t)st.st_mtime * 1099511628211ull);
    for (int i = 0; i < 8; i++) { h ^= (usize  >> (8 * i)) & 0xffu; h *= 1099511628211ull; }
    for (int i = 0; i < 8; i++) { h ^= (csize  >> (8 * i)) & 0xffu; h *= 1099511628211ull; }
    for (int i = 0; i < 8; i++) { h ^= (extra  >> (8 * i)) & 0xffu; h *= 1099511628211ull; }
    return h;
}
bool tl_zip_data_cached(const tl_zip *z, const tl_zip_entry *e, const char *apk,
                        size_t limit, const uint8_t **out, size_t *out_len, bool *owned,
                        char *err, size_t errlen)
{
    *out = NULL; *out_len = 0; *owned = false;
    if (!z || !e) { if (err && errlen) snprintf(err, errlen, "no entry"); return false; }
    if (e->method != 8)
        return tl_zip_data(z, e, limit, out, out_len, owned, err, errlen);   /* stored: read in place, nothing to cache */

    char dir[1100], cpath[1300];
    snprintf(dir, sizeof(dir), "%s/.lib-cache", tl_data_dir());
    mkdir(dir, 0755);
    snprintf(cpath, sizeof(cpath), "%s/%016llx.bin", dir,
             (unsigned long long)load_entry_identity(apk, e->name, e->usize, e->csize));

    struct stat st;
    if (e->usize <= limit && stat(cpath, &st) == 0 && (uint64_t)st.st_size == e->usize) {
        unsigned long long t0 = tl_load_now_ns();
        FILE *f = fopen(cpath, "rb");
        uint8_t *buf = e->usize ? malloc((size_t)e->usize) : NULL;
        bool ok = f && buf && fread(buf, 1, (size_t)e->usize, f) == (size_t)e->usize;
        if (f) fclose(f);
        if (ok) {
            tl_load_add(TL_LOAD_COPY, tl_load_now_ns() - t0, e->usize);
            *out = buf; *out_len = (size_t)e->usize; *owned = true;
            return true;
        }
        free(buf);                            /* a torn or short file: never serve it, inflate afresh */
    }

    unsigned long long t0 = tl_load_now_ns();
    if (!tl_zip_data(z, e, limit, out, out_len, owned, err, errlen)) return false;
    tl_load_add(TL_LOAD_INFLATE, tl_load_now_ns() - t0, *out_len);

    if (e->usize <= limit) {
        char tmp[1320];
        snprintf(tmp, sizeof(tmp), "%s.part", cpath);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            bool wrote = fwrite(*out, 1, *out_len, f) == *out_len;
            fclose(f);
            if (wrote) { if (rename(tmp, cpath) != 0) unlink(tmp); }
            else unlink(tmp);
        }
    }
    return true;
}

/* A file holding `content`, already unlinked. Where it can be made depends on the host: iOS gives an app no /tmp. */
static int synth_file_n(const void *content, size_t n);
static int synth_file(const char *content) { return synth_file_n(content, strlen(content)); }
static int synth_file_n(const void *content, size_t n)
{
    const char *dirs[3] = { g_data_dir[0] ? g_data_dir : NULL, getenv("TMPDIR"), "/tmp" };
    char tmpl[1100];
    int fd = -1;
    for (int i = 0; i < 3 && fd < 0; i++) {
        if (!dirs[i]) continue;
        snprintf(tmpl, sizeof(tmpl), "%s/husk-synth-XXXXXX", dirs[i]);
        fd = mkstemp(tmpl);
    }
    if (fd < 0) { tl_log_line("bionic: no writable directory for a synthetic file (%s)", strerror(errno)); return -1; }
    unlink(tmpl);
    if (write(fd, content, n) != (ssize_t)n) { close(fd); return -1; }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

static const char *synth_content(const char *path, char *buf, size_t n)
{
    if (!strcmp(path, "/proc/cpuinfo")) {
        buf[0] = 0;
        long ncpu = getenv("TL_NCPU") ? atol(getenv("TL_NCPU")) : sysconf(_SC_NPROCESSORS_ONLN);
        for (long i = 0; i < ncpu; i++) {
            size_t l = strlen(buf);
            snprintf(buf + l, n - l, "processor\t: %ld\nBogoMIPS\t: 48.00\nFeatures\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp\n"
                     "CPU implementer\t: 0x61\nCPU architecture: 8\nCPU variant\t: 0x0\nCPU part\t: 0x0\nCPU revision\t: 0\n\n", i);
        }
        return buf;
    }
    if (!strcmp(path, "/proc/meminfo")) {
        snprintf(buf, n, "MemTotal:       11722000 kB\nMemFree:         3000000 kB\nMemAvailable:    5000000 kB\nBuffers:           10000 kB\nCached:          2000000 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n");
        return buf;
    }
    if (!strncmp(path, "/sys/devices/system/cpu/", 24) && (strstr(path, "/present") || strstr(path, "/possible") || strstr(path, "/online"))) {
        snprintf(buf, n, "0-%ld\n", (getenv("TL_NCPU") ? atol(getenv("TL_NCPU")) : sysconf(_SC_NPROCESSORS_ONLN)) - 1);
        return buf;
    }
    if (!strcmp(path, "/proc/self/maps") || (!strncmp(path, "/proc/", 6) && strstr(path, "/maps") && !strcmp(strstr(path, "/maps"), "/maps"))) {
        size_t tl_ld_maps(char *buf, size_t n, const char *libdir);
        char dir[1024]; snprintf(dir, sizeof(dir), "%s/lib", tl_data_dir());
        tl_ld_maps(buf, n, dir);
        return buf;
    }
    if (!strcmp(path, "/proc/self/status")) {
        snprintf(buf, n, "Name:\tapp_process64\nState:\tR (running)\nTgid:\t%d\nPid:\t%d\nPPid:\t1\nUid:\t10001\t10001\t10001\t10001\nThreads:\t8\nVmRSS:\t  300000 kB\n", getpid(), getpid());
        return buf;
    }
    return NULL;
}

/* The directories the synthesised files above live in. A caller that checks the *containing* directory before opening a
 * file (Mono's FileStream does, and reports a missing one as "DirectoryNotFoundException") must find them, or it never
 * reaches synth_content()/b_open(). Nothing here shadows a real host path: iOS has no /proc and no /sys. */
static bool synth_dir(const char *p)
{
    static const char *const dirs[] = {
        "/proc", "/proc/self",
        "/sys", "/sys/devices", "/sys/devices/system", "/sys/devices/system/cpu",
    };
    if (!p) return false;
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) if (!strcmp(p, dirs[i])) return true;
    return false;
}

/* A file for a path whose contents are made up (/proc/cpuinfo ...), or -1 when the path is real. */
int tl_synth_open(const char *path)
{
    /* the auxiliary vector, as the kernel gives it (Go's runtime in a library reads its page size here): the values getauxval answers */
    if (!strcmp(path, "/proc/self/auxv") || (!strncmp(path, "/proc/", 6) && strstr(path, "/auxv") && !strcmp(strstr(path, "/auxv"), "/auxv"))) {
        unsigned long tl_bionic_getauxval(unsigned long type);
        static uint8_t random16[16]; static int made; if (!made) { arc4random_buf(random16, 16); made = 1; }
        uint64_t v[] = { 6, 16384, 16, tl_bionic_getauxval(16), 26, tl_bionic_getauxval(26), 17, 100, 25, (uint64_t)(uintptr_t)random16,
                         23, 0, 51, 4096, 0, 0 };
        return synth_file_n(v, sizeof(v));
    }
    static __thread char content[65536];
    const char *s = synth_content(path, content, sizeof(content));
    return s ? synth_file(s) : -1;
}

static char g_shared_storage[1024];
void tl_set_shared_storage(const char *dir) { snprintf(g_shared_storage, sizeof(g_shared_storage), "%s", dir ? dir : ""); }

/* The trusted root certificates (one PEM file per certificate, named by subject hash), where Android keeps them: TLS libraries that
 * read them from there (Dart's, in Flutter apps) find the copy the app carries. */
static char g_cacerts[1024];
void tl_set_cacerts_dir(const char *dir) { snprintf(g_cacerts, sizeof(g_cacerts), "%s", dir ? dir : ""); }

/* The system's fonts (/system/fonts) and their list (/system/etc/fonts.xml, font_fallback.xml), which text engines read to find a
 * typeface (Skia, in Flutter apps): a copy the app carries, laid out as fonts/ and etc/ under one directory. */
static char g_sysfonts[1024];
void tl_set_system_fonts_dir(const char *dir) { snprintf(g_sysfonts, sizeof(g_sysfonts), "%s", dir ? dir : ""); }
/* Fonts the app downloaded later (the Chinese, Japanese and Korean ones the bundle leaves out, #95): a /system/fonts file the bundle
 * does not have is looked for here too, and fonts.xml already names them. */
static char g_extrafonts[1024];
void tl_set_extra_fonts_dir(const char *dir) { snprintf(g_extrafonts, sizeof(g_extrafonts), "%s", dir ? dir : ""); }

/* -------------------------------------------------- the Macintosh that Ren'Py thinks it is
 * Ren'Py's platform check (renpy/__init__.py) tests mac_ver() BEFORE the "ANDROID_PRIVATE in os.environ" test that sets
 * renpy.android. The host's readable /System/Library/CoreServices/SystemVersion.plist makes mac_ver() answer, so Ren'Py
 * calls itself macintosh and the Android branch is never reached -- renpy.loader never indexes the APK and the first
 * load_module fails. So for a Ren'Py app ONLY (the flag is set by tl_renpy_prepare) the guest is told that one host file
 * is absent, mac_ver() falls through and renpy.android is true. Nothing else reads it; no other engine sets it. */
static bool g_renpy_android;
void tl_renpy_android_on(void) { g_renpy_android = true; }
static bool renpy_hidden_macver(const char *p)
{
    return g_renpy_android && p && (strstr(p, "/CoreServices/SystemVersion.plist") != NULL);
}

/* A game's expansion file (Android/obb/<package>/main.<version>.<package>.obb) that is there: said once, naming where it
 * was found. Skipped for a non-.obb under /obb and for a target that does not exist (a game creating the file itself must
 * not be reported as having found one). */
static void note_obb_used(const char *rest, const char *full)
{
    static bool said;
    const char *dot;
    struct stat st;
    if (said || !rest || !strstr(rest, "/obb/")) return;
    dot = strrchr(rest, '.');
    if (!dot || strcmp(dot, ".obb")) return;
    if (stat(full, &st) != 0) return;
    said = true;
    tl_log_line("husk: the app's expansion file (OBB) is at %s (%lld bytes)", full, (long long)st.st_size);
}

const char *tl_path_resolve(const char *path, char *buf, size_t n)
{
    if (!path) return path;
    if (g_sysfonts[0]) {
        if (!strncmp(path, "/system/fonts", 13) && (path[13] == 0 || path[13] == '/')) {
            snprintf(buf, n, "%s/fonts%s", g_sysfonts, path + 13);
            if (g_extrafonts[0] && path[13] == '/' && access(buf, F_OK) != 0) {
                char extra[1100];
                snprintf(extra, sizeof(extra), "%s%s", g_extrafonts, path + 13);
                if (access(extra, F_OK) == 0) snprintf(buf, n, "%s", extra);
            }
            return buf;
        }
        if (!strcmp(path, "/system/etc/fonts.xml") || !strcmp(path, "/system/etc/font_fallback.xml")) { snprintf(buf, n, "%s/etc%s", g_sysfonts, path + 11); return buf; }
    }
    if (g_cacerts[0]) {
        static const char *const roots[] = { "/system/etc/security/cacerts", "/apex/com.android.conscrypt/cacerts" };
        for (int i = 0; i < 2; i++) {
            size_t l = strlen(roots[i]);
            if (!strncmp(path, roots[i], l) && (path[l] == 0 || path[l] == '/')) { snprintf(buf, n, "%s%s", g_cacerts, path + l); return buf; }
        }
    }
    /* /system/lib64/libdl.so and the like: the libraries Husk provides itself are files there, as on a phone, so loaders that look
     * for a dependency on disk before loading it (Facebook's SoLoader, in React Native apps) find them. Each is a small ELF stub;
     * loading it by that path loads Husk's own. */
    {
        static const char *const libdirs[] = { "/system/lib64/", "/System/lib64/", "/vendor/lib64/", "/system/lib/" };
        for (int i = 0; i < 4; i++) {
            size_t l = strlen(libdirs[i]);
            bool tl_bionic_is_system_lib(const char *soname);
            if (!strncmp(path, libdirs[i], l) && !strchr(path + l, '/') && tl_bionic_is_system_lib(path + l)) {
                snprintf(buf, n, "%s/.husk-system-lib2.so", tl_data_dir());
                struct stat st;
                if (stat(buf, &st) != 0) {
                    /* a minimal arm64 shared object: a header, a load segment and a dynamic section (SONAME only), which ELF
                     * readers such as SoLoader's MinElf parse to list a library's dependencies (none) */
                    uint8_t elf[0x110]; memset(elf, 0, sizeof(elf));
                    #define W16(o, v) do { elf[o] = (uint8_t)(v); elf[(o) + 1] = (uint8_t)((v) >> 8); } while (0)
                    #define W32(o, v) do { W16(o, (v) & 0xffff); W16((o) + 2, ((uint32_t)(v)) >> 16); } while (0)
                    #define W64(o, v) do { W32(o, (uint32_t)(v)); W32((o) + 4, (uint32_t)((uint64_t)(v) >> 32)); } while (0)
                    static const uint8_t ident[16] = { 0x7f, 'E', 'L', 'F', 2, 1, 1, 0 };
                    memcpy(elf, ident, 16);
                    W16(16, 3); W16(18, 0xb7); W32(20, 1); W64(32, 64); W16(52, 64); W16(54, 56); W16(56, 2); W16(58, 64);
                    W32(64, 1); W32(68, 4); W64(72, 0); W64(80, 0); W64(88, 0); W64(96, sizeof(elf)); W64(104, sizeof(elf)); W64(112, 0x1000);
                    W32(120, 2); W32(124, 6); W64(128, 0xb0); W64(136, 0xb0); W64(144, 0xb0); W64(152, 64); W64(160, 64); W64(168, 8);
                    W64(0xb0, 5); W64(0xb8, 0xf0);                  /* DT_STRTAB */
                    W64(0xc0, 10); W64(0xc8, 0x20);                 /* DT_STRSZ */
                    W64(0xd0, 14); W64(0xd8, 1);                    /* DT_SONAME */
                    memcpy(elf + 0xf1, "libhusk-system.so", 17);
                    #undef W64
                    #undef W32
                    #undef W16
                    FILE *f = fopen(buf, "wb");
                    if (f) { fwrite(elf, 1, sizeof(elf), f); fclose(f); }
                }
                return buf;
            }
        }
    }
    /* /data/user/0/<package> and /data/data/<package>: the app's own folder, as Android names it -- short, where the real one on a
       phone is ~150 characters (p4a's launcher allows its script path 128) */
    {
        static const char *const roots[] = { "/data/user/0/", "/data/data/" };
        for (int i = 0; i < 2; i++) {
            size_t l = strlen(roots[i]);
            if (strncmp(path, roots[i], l) || !path[l] || path[l] == '/') continue;
            const char *rest = strchr(path + l, '/');
            snprintf(buf, n, "%s%s", tl_data_dir(), rest ? rest : "");
            return buf;
        }
    }
    /* /data/local/tmp: the world-writable scratch folder (JNA unpacks itself there): a folder of the app's own */
    if (!strncmp(path, "/data/local/tmp", 15) && (path[15] == 0 || path[15] == '/')) {
        snprintf(buf, n, "%s/cache/local-tmp", tl_data_dir());
        mkdir(buf, 0777);
        snprintf(buf, n, "%s/cache/local-tmp%s", tl_data_dir(), path + 15);
        return buf;
    }
    const char *pkg = "/data/data/";
    if (!strncmp(path, pkg, strlen(pkg))) {
        const char *rest = strchr(path + strlen(pkg), '/');
        snprintf(buf, n, "%s%s", tl_data_dir(), rest ? rest : "");
        return buf;
    }
    if (!strncmp(path, "/data/user/0/", 13)) {
        const char *rest = strchr(path + 13, '/');
        snprintf(buf, n, "%s%s", tl_data_dir(), rest ? rest : "");
        return buf;
    }
    if (!strncmp(path, "/sdcard", 7) || !strncmp(path, "/storage/emulated/0", 19)) {
        const char *rest = !strncmp(path, "/sdcard", 7) ? path + 7 : path + 19;
        /* Shared storage (anything but an app's own Android/data) is one folder for every game when the app gave one, which
         * the player can fill from Files. Android/obb is included on purpose: an expansion file is the one thing a player
         * supplies by hand, and a game asking Android for its OBB directory is handed this same path. */
        if (g_shared_storage[0] && strncmp(rest, "/Android/data", 13)) {
            char own[1024]; struct stat st;
            snprintf(own, sizeof(own), "%s/sdcard%s", tl_data_dir(), rest);
            if (rest[0] && rest[1] && stat(own, &st) == 0) { snprintf(buf, n, "%s", own); note_obb_used(rest, buf); return buf; }
            snprintf(buf, n, "%s%s", g_shared_storage, rest);
            note_obb_used(rest, buf);
            return buf;
        }
        snprintf(buf, n, "%s/sdcard%s", tl_data_dir(), rest);
        /* An expansion file (Android/obb/<package>/main.<version>.<package>.obb) not in the app's data: the one Husk's store
         * downloaded with the app, kept beside its APKs in obb/ */
        if (!strncmp(rest, "/Android/obb/", 13)) {
            struct stat st;
            const char *apk = tl_ld_main_apk(), *file = strrchr(rest, '/');
            if (stat(buf, &st) != 0 && apk && file && file[1]) {
                char beside[1024];
                const char *slash = strrchr(apk, '/');
                snprintf(beside, sizeof(beside), "%.*s/obb%s", slash ? (int)(slash - apk) : 0, apk, file);
                if (stat(beside, &st) == 0) snprintf(buf, n, "%s", beside);
            }
        }
        note_obb_used(rest, buf);
        return buf;
    }
    return path;
}


/* ------------------------------------------------------------ virtual files */

/* A file that is really a stretch of another: a UE4 OBB a repackaged APK carries inside itself, shown to the game under
 * its usual name with reads going to the right place in the APK. Matching is by file name. */
typedef struct { char name[160]; char host[1024]; uint64_t off, size; } vfile;
/* A descriptor that is really a stretch of an APK entry: a stored jar:file: entry served in place. `on` says the
 * descriptor is served from the entry now; `was` remembers a descriptor whose redirect was DROPPED (closed, or replaced
 * by dup2/dup3) together with the entry it served, so a later read on that number can be NAMED instead of surfacing only
 * as the guest's nameless generic I/O error (Unity's "Win32 IO returned ERROR_GEN_FAILURE"). `name` is diagnostic only. */
typedef struct { bool on, was; uint64_t off, size, pos; char name[96]; } vfd;
static vfile g_vfiles[8];
static int g_nvfiles;
static vfd g_vfd[4096];

void tl_vfile_add(const char *guest_name, const char *host_path, uint64_t off, uint64_t size)
{
    if (g_nvfiles >= 8) return;
    snprintf(g_vfiles[g_nvfiles].name, sizeof(g_vfiles[0].name), "%s", guest_name);
    snprintf(g_vfiles[g_nvfiles].host, sizeof(g_vfiles[0].host), "%s", host_path);
    g_vfiles[g_nvfiles].off = off; g_vfiles[g_nvfiles].size = size;
    g_nvfiles++;
}

static const vfile *vfile_find(const char *path)
{
    if (!g_nvfiles || !path) return NULL;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (int i = 0; i < g_nvfiles; i++) if (!strcmp(base, g_vfiles[i].name)) return &g_vfiles[i];
    return NULL;
}
static bool vfd_is(int fd) { return fd >= 0 && fd < 4096 && g_vfd[fd].on; }

/* The redirect belongs to the descriptor, so it follows it exactly as the kernel's file description does: dup/F_DUPFD
 * make a second descriptor onto the same entry (both redirect); dup2 replacing a descriptor drops the old one; a raw
 * close must leave nothing behind, or the next file to land on that number is served the old APK's bytes. Every one of
 * those goes through these two, so the table cannot drift from the descriptors it describes. */
static void vfd_copy(int newfd, int oldfd)
{
    if (newfd >= 0 && newfd < 4096) g_vfd[newfd] = (oldfd >= 0 && oldfd < 4096 && g_vfd[oldfd].on) ? g_vfd[oldfd] : (vfd){ 0 };
}
/* Drop a descriptor's redirect, but keep the entry's name and geometry as a tombstone (`was`), never a live redirect:
 * an entry that outlived its descriptor must never serve again, yet a read that fails on that number should still be
 * able to say which entry it used to be. */
static void vfd_drop(int fd)
{
    if (fd < 0 || fd >= 4096) return;
    if (g_vfd[fd].on) { g_vfd[fd].on = false; g_vfd[fd].was = true; g_vfd[fd].pos = 0; }
}
/* The layer's one place for recording a redirect, so the descriptor's name is remembered wherever one is set and a
 * failure can name it. */
static void vfd_set(int fd, uint64_t off, uint64_t size, const char *name)
{
    if (fd < 0 || fd >= 4096) return;
    g_vfd[fd] = (vfd){ .on = true, .was = false, .off = off, .size = size, .pos = 0 };
    snprintf(g_vfd[fd].name, sizeof(g_vfd[fd].name), "%s", name ? name : "");
}
/* A descriptor number just handed out by an open that does NOT redirect it: whatever the layer remembered about that
 * number belongs to another file now. The kernel never reuses a number that is still open, so this cannot drop a live
 * redirect -- it closes the other half of the invariant: a redirect that outlived its descriptor must not be applied to
 * the next file to land on the number. */
static void vfd_take_over(int fd)
{
    if (fd >= 0 && fd < 4096) g_vfd[fd] = (vfd){ 0 };
}

/* ---------------------------------------- naming a failed read on a redirected descriptor
 *
 * The entry's line prints at OPEN time, so a build can log a served entry whose descriptor is still read wrong -- and the
 * guest reports only System.IO.IOException: "Win32 IO returned ERROR_GEN_FAILURE", a generic code with no errno (Mono's
 * file_read turns the read() failure into one). The one place that still knows the errno is here, so a read/pread/seek on
 * a redirected descriptor that FAILS is logged once: which descriptor and call, the offset and length, where inside the
 * APK that lands, the entry and its size, and the host errno beside its guest number. A descriptor whose redirect was
 * dropped says that too -- the shape that reads the APK's own bytes at the APK's own offset, or fails outright. */
static void vfd_note_failed(int fd, const char *call, const vfd *v, uint64_t off, size_t len, int host_errno)
{
    static bool said[4096];
    if (fd < 0 || fd >= 4096 || said[fd]) return;
    said[fd] = true;
    tl_log_line("husk: virtual file: %s on redirected fd %d FAILED: asked for %zu bytes at %llu of the entry, the APK's byte %llu (entry %s, %llu bytes), errno %d (guest %d)",
                call, fd, len, (unsigned long long)off, (unsigned long long)(v->off + off),
                v->name[0] ? v->name : "(unnamed)", (unsigned long long)v->size, host_errno,
                tl_errno_to_guest(host_errno));
}
/* A read on a descriptor the layer DID redirect and no longer is: its redirect was dropped when the descriptor was
 * closed or replaced. Saying this separates "the layer never served the entry" from "it served it once and the guest kept
 * reading a number whose redirect was gone" -- identical from Unity's side. */
static void vfd_note_dropped(int fd, const char *call, long r, int host_errno)
{
    static bool said[4096];
    if (fd < 0 || fd >= 4096 || !g_vfd[fd].was || said[fd]) return;
    said[fd] = true;
    if (r < 0)
        tl_log_line("husk: virtual file: %s on fd %d FAILED with NO redirect: this descriptor last served the APK entry %s (%llu bytes), and its redirect was dropped when the descriptor was closed or replaced. errno %d (guest %d). Unity reports a read like this as \"Win32 IO returned ERROR_GEN_FAILURE\".",
                    call, fd, g_vfd[fd].name[0] ? g_vfd[fd].name : "(unnamed)", (unsigned long long)g_vfd[fd].size,
                    host_errno, tl_errno_to_guest(host_errno));
    else
        tl_log_line("husk: virtual file: %s on fd %d returned %ld bytes with NO redirect: this descriptor last served the APK entry %s (%llu bytes) and its redirect was dropped when the descriptor was closed or replaced, so these are the APK's own bytes at the APK's own offset -- a wrong file, silently.",
                    call, fd, r, g_vfd[fd].name[0] ? g_vfd[fd].name : "(unnamed)", (unsigned long long)g_vfd[fd].size);
}

/* ------------------------------------------------- a kernel write into a guest buffer, and where it lands
 *
 * A read hands the KERNEL an address to write the bytes into. That address is the guest's own (the guest runs natively,
 * so a guest address is already a host address) and is passed through unchanged. So a read can fail with EFAULT (14)
 * with the file, offset and descriptor all correct: errno 14 is the kernel saying it will not write THERE (EBADF/EINVAL/
 * EIO/ESPIPE are a wrong descriptor, offset or file; 14 is the destination).
 *
 * The JIT region (husk-tl-xmem.c) is one set of pages seen through two views a fixed delta apart. A guest store into the
 * EXECUTABLE view is carried out through the other view (husk-tl-codewrite.c put()), so to the guest the memory is
 * writable -- but a kernel read()/pread() writes through the page tables, meets the view's real protection (read+execute)
 * and answers EFAULT. The two views are the same pages, so the kernel is sent to the writable view at the fixed delta and
 * writes the very bytes the guest reads back at its own address -- the same move codewrite.c makes for a store. */
static void *kernel_dst(void *p, size_t n)
{
    if (n && tl_xmem_is_rx(p) && tl_xmem_is_rx((char *)p + n - 1)) {
        static int said;
        if (said++ < 1)
            tl_log_line("husk: read into the JIT region's EXECUTABLE view: destination %p (%zu bytes) is not writable by the kernel, so the read goes to the writable view %p (same pages, delta %td) -- the guest reads the result back at %p",
                        p, n, (void *)((char *)p + tl_xmem_delta()), tl_xmem_delta(), p);
        return (char *)p + tl_xmem_delta();
    }
    return p;
}

/* The iovec form of the same translation, for readv/preadv: any iovec whose base is the JIT region's executable view is
 * sent to the writable view so the kernel can fill it. Up to 32 iovecs; more is passed through unchanged. Returns whether
 * anything was moved. */
static int iov_dst(const struct iovec *v, int n, struct iovec *out, int cap)
{
    if (n <= 0 || n > cap) return 0;
    int moved = 0;
    for (int i = 0; i < n; i++) {
        out[i] = v[i];
        void *d = kernel_dst(out[i].iov_base, out[i].iov_len);
        if (d != out[i].iov_base) { out[i].iov_base = d; moved = 1; }
    }
    return moved;
}

/* A host-side verdict on the address a read was told to write into, so an EFAULT stops being a guess: is the address
 * mapped at all, with what protection, and is it the JIT region's executable view? Printed with the destination pointer
 * and length. Only EFAULT reaches it (the only errno about the destination). */
static void vfd_note_dst(bool redirected, int fd, const char *call, const void *p, size_t n, int host_errno)
{
    if (host_errno != EFAULT) return;
    static int said;
    if (said >= 16) return;                        /* enough to name it; a storm of them must not bury the log */
    said++;
    vm_address_t a = (vm_address_t)p; vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
    kern_return_t kr = vm_region_64(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj);
    char region[160];
    if (kr == KERN_SUCCESS) {
        char prot[4], mx[4];
        prot[0] = (info.protection & VM_PROT_READ) ? 'r' : '-'; prot[1] = (info.protection & VM_PROT_WRITE) ? 'w' : '-'; prot[2] = (info.protection & VM_PROT_EXECUTE) ? 'x' : '-'; prot[3] = 0;
        mx[0] = (info.max_protection & VM_PROT_READ) ? 'r' : '-'; mx[1] = (info.max_protection & VM_PROT_WRITE) ? 'w' : '-'; mx[2] = (info.max_protection & VM_PROT_EXECUTE) ? 'x' : '-'; mx[3] = 0;
        snprintf(region, sizeof region, "vm_region: mapped at %p prot %s max %s, %s", (void *)(uintptr_t)a, prot, mx,
                 info.protection & VM_PROT_WRITE ? "writable" : "NOT writable");
    } else {
        snprintf(region, sizeof region, "vm_region: NOT MAPPED (kern_return %d)", kr);
    }
    const char *where =
        tl_xmem_is_rx(p)        ? "the JIT region's EXECUTABLE view (the kernel will not write it; the guest's stores there are emulated through the writable view at delta)" :
        tl_xmem_contains(p)     ? "the JIT region's writable (guest-facing) view" :
        (kr == KERN_SUCCESS)    ? "an ordinary mapping" : "an unmapped address";
    tl_log_line("husk: virtual file: %s on %sfd %d FAILED with errno %d (guest %d): the DESTINATION is the fault, not the file, the offset or the descriptor -- buffer %p (%zu bytes) handed to the kernel unchanged is %s [%s]",
                call, redirected ? "redirected " : "", fd, host_errno, tl_errno_to_guest(host_errno), p, n, where, region);
}

/* b_dup3 (husk-tl-bionic-ldbl.c) must route through b_dup2, so this file's descriptor->entry table follows dup3 exactly as
 * it follows dup/dup2/F_DUPFD (see vfd_copy); it looks b_dup2 up by name for that. */

static void ftrace_open_jar(const char *path, int fd)
{
    static int tr = -1;
    if (tr < 0) tr = getenv("TL_FILE_TRACE") ? atoi(getenv("TL_FILE_TRACE")) : 0;
    if (tr) tl_log_line("file: open(%s) -> %d (inside the APK)", path, fd);
}

/* ------------------------------------------------------------ files inside an APK */

/*
 * "jar:file:///.../base.apk!/assets/aa/Android/catalog.json": how Java's JarURLConnection, and Unity's Addressables after
 * it, name a file inside the APK. The guest hands such a path straight to stat() and open(), sometimes with a current
 * directory glued in front. A stored entry is served in place as a stretch of the APK (the virtual-file machinery above);
 * a compressed one is inflated once into the data folder and opened from there.
 */
typedef struct { bool found, dir; char apk[1024]; uint64_t off, size, csize; uint16_t method; char entry[512]; } jar_entry;
static pthread_mutex_t g_jar_lock = PTHREAD_MUTEX_INITIALIZER;
static tl_zip g_jar_zip;
static char g_jar_apk[1024];

static bool jar_lookup(const char *p, jar_entry *out);
/* The native library folder holds an empty stand-in for each library the APKs carry (so File.exists()/findLibrary() find
 * them); opened or looked at, one is the APK's entry (Unity copies libil2cpp.usym.so out of there). */
static bool lib_standin(const char *p, jar_entry *out)
{
    const char *dd = tl_data_dir();
    /* iOS names the app's container both /var/mobile/... and /private/var/mobile/... (realpath, Qt's plugin scan): either is it */
    if (dd && !strncmp(p, "/private/var/", 13) && !strncmp(dd, "/var/", 5)) p += 8;
    else if (dd && !strncmp(p, "/var/", 5) && !strncmp(dd, "/private/var/", 13)) dd += 8;
    size_t dl = dd ? strlen(dd) : 0;
    if (!dl || strncmp(p, dd, dl) || strncmp(p + dl, "/lib/", 5) || strchr(p + dl + 5, '/')) return false;
    const char *name = p + dl + 5;
    size_t nl = strlen(name);
    if (nl < 4 || strcmp(name + nl - 3, ".so")) return false;
    struct stat st;
    if (stat(p, &st) != 0 || st.st_size != 0) return false;
    const char *tl_ld_lib_apk(const char *name);
    const char *apk = tl_ld_lib_apk(name);
    if (!apk) return false;
    char jar[1400]; snprintf(jar, sizeof(jar), "jar:file://%s!/lib/arm64-v8a/%s", apk, name);
    return jar_lookup(jar, out) && out->found;
}
/* A URL fragment's percent-encoding ("%2F" for '/'), decoded into out. Anything not a complete "%XX" is copied as it
 * stands. */
static void jar_unescape(const char *s, const char *end, char *out, size_t n)
{
    size_t k = 0;
    for (const char *c = s; c < end && k + 1 < n; c++) {
        if (c[0] == '%' && c + 2 < end && isxdigit((unsigned char)c[1]) && isxdigit((unsigned char)c[2])) {
            char hex[3] = { c[1], c[2], 0 }; out[k++] = (char)strtol(hex, NULL, 16); c += 2;
        } else out[k++] = *c;
    }
    out[k] = 0;
}

static bool jar_lookup(const char *p, jar_entry *out)
{
    if (p && p[0] == '/' && lib_standin(p, out)) return true;
    memset(out, 0, sizeof(*out));
    /* "jar:file:<apk>!/<entry>" and "/jar:file:<apk>!/<entry>" both name an entry in an APK; the "jar:file:" is found
     * wherever it sits, so either spelling -- and a current directory glued in front of one -- is understood. The "//" of
     * a plain file URL is consumed too. */
    const char *q = p ? strstr(p, "jar:file:") : NULL;
    if (!q) return false;
    q += 9;
    while (q[0] == '/' && q[1] == '/') q++;
    const char *bang = strstr(q, "!/");
    if (!bang || (size_t)(bang - q) >= sizeof(out->apk)) return true;          /* a jar path, but not one that names an entry */
    jar_unescape(q, bang, out->apk, sizeof(out->apk));
    /* The APK is resolved the way every other path is: the layer hands an app its own real APK path (getPackageCodePath /
     * sourceDir), but a path the layer maps -- or one the guest spelled itself -- is put through the same mapping, so the file
     * is found. A real path is returned unchanged. */
    {
        char rb[1024];
        const char *rp = tl_path_resolve(out->apk, rb, sizeof(rb));
        if (rp && rp != out->apk) snprintf(out->apk, sizeof(out->apk), "%s", rp);
    }
    jar_unescape(bang + 2, bang + 2 + strlen(bang + 2), out->entry, sizeof(out->entry));
    pthread_mutex_lock(&g_jar_lock);
    if (strcmp(g_jar_apk, out->apk) != 0) {
        if (g_jar_apk[0]) tl_zip_close(&g_jar_zip);
        g_jar_apk[0] = 0;
        char err[160];
        if (tl_zip_open(&g_jar_zip, out->apk, err, sizeof(err))) snprintf(g_jar_apk, sizeof(g_jar_apk), "%s", out->apk);
    }
    const tl_zip_entry *e = g_jar_apk[0] ? tl_zip_find(&g_jar_zip, out->entry) : NULL;
    if (e && e->local_offset + 30 <= g_jar_zip.size) {
        const uint8_t *l = g_jar_zip.map + e->local_offset;
        uint64_t data = e->local_offset + 30ull + (uint64_t)(l[26] | (l[27] << 8)) + (uint64_t)(l[28] | (l[29] << 8));
        out->found = true; out->off = data; out->size = e->usize; out->csize = e->csize; out->method = e->method;
    } else if (g_jar_apk[0]) {
        /* No entry by that name: a folder, if any entry lies under it (zips seldom list folders themselves). */
        size_t len = strlen(out->entry);
        while (len && out->entry[len - 1] == '/') out->entry[--len] = 0;
        for (size_t i = 0; len && i < g_jar_zip.count; i++) {
            const char *name = g_jar_zip.entries[i].name;
            if (!strncmp(name, out->entry, len) && name[len] == '/') { out->found = out->dir = true; break; }
        }
    }
    pthread_mutex_unlock(&g_jar_lock);
    return true;
}

/*
 * One app's host world, forgotten at the session boundary, so the next app starts from nothing of the last one. Called
 * from husk_tl_session_close after the app was asked to close and before a second app is launched. Every item is the
 * layer's OWN bookkeeping -- no guest memory, no mapping unmapped, no guest descriptor closed -- so a quarantined thread
 * cannot be reading through anything this drops:
 *
 *   g_renpy_android        Ren'Py's platform answer, set ONLY for a Ren'Py app. Left set, every later app (Ren'Py or
 *                          not) is lied to about that host file.
 *   g_vfiles / g_nvfiles   the OBB-inside-an-APK name table (husk-tl-nativeactivity.c). Append-only and capped at 8, so
 *                          it must be cleared per app: a name the old app registered would otherwise resolve to the old
 *                          app's bytes.
 *   g_vfd                  the descriptor -> APK-entry redirection table. An entry must not outlive its descriptor, or the
 *                          next app's descriptor could land on that number and be served the old APK's bytes. Swept here,
 *                          but only where the descriptor is genuinely gone (F_GETFD fails with EBADF); a live one is left.
 *   g_jar_zip / g_jar_apk  the jar:file: entry cache. Keyed by APK path, so it self-corrects; dropped so the ended app's
 *                          APK is not held open longer than needed. The layer's own tl_zip handle, never a guest fd.
 */
void tl_io_reset_for_new_app(void)
{
    int nvfiles = g_nvfiles;
    g_renpy_android = false;
    g_nvfiles = 0;

    int swept = 0;
    for (int fd = 0; fd < 4096; fd++) {
        if (g_vfd[fd].on) {
            if (fcntl(fd, F_GETFD) == -1 && errno == EBADF) { g_vfd[fd] = (vfd){ 0 }; swept++; }
        } else if (g_vfd[fd].was) {
            g_vfd[fd] = (vfd){ 0 };                 /* a tombstone names the ended app's entry: it goes with the app (see vfd_drop) */
        }
    }

    int jar = 0;
    pthread_mutex_lock(&g_jar_lock);
    if (g_jar_apk[0]) { tl_zip_close(&g_jar_zip); g_jar_apk[0] = 0; jar = 1; }
    pthread_mutex_unlock(&g_jar_lock);

    tl_log_line("husk: app state reset: renpy platform flag off, %d virtual-file name(s) dropped, %d stale descriptor redirect(s) swept, jar cache closed=%d",
                nvfiles, swept, jar);
    /* The reads of the app that just ended, oldest first, once at the boundary -- then forget them, so the next app's ring is its own. */
    tl_asset_ring_dump("close");
    tl_asset_ring_reset();
    /* Where the load path spent its time, once, at the session boundary -- never per event. */
    tl_load_summary("close");
}

/*
 * The app's OWN reads leading up to a freeze, the PAIRED instrument. The stall probe's verdict, when no parked thread
 * holds a layer lock, points here: at the app's onResume/launch and its asset/asset-file reads.
 *
 * It is an ALWAYS-ARMED, fixed-capacity ring, not a swap-gated logger, because the old trace turned ON only at the moment
 * a stall was DETECTED (tl_swap_in_progress_set(1) inside ss_probe): by then the app had already stopped reading, so it
 * could only ever record the silence it was meant to explain (the newest log has the swap-stall line and ZERO asset
 * lines). The ring holds the most recent reads whatever the run, so a stall can dump what came BEFORE the freeze.
 *
 * The hot path is one relaxed atomic fetch_add (this read's sequence number) and a bounded copy of the entry name, the
 * ok/err and the reading thread into slot (seq-1) % N; no allocation, no lock, no log line while the app runs. The
 * sequence is published LAST (release), so a dumper never treats a half-written name as current. TL_ASSET_TRACE=1 still
 * logs every read as it happens (a desktop aid); it is no longer what makes the record exist. `ok` -1 = attempt, 1 =
 * served, 0 = failed.
 */
#define TL_ASSET_RING 64
#define TL_ASSET_NAME 160
typedef struct {
    unsigned long seq;              /* this read's number, published after the fields below (all access is through __atomic_*); 0 = never written */
    int ok, err;
    char name[TL_ASSET_NAME];
    unsigned long tid;              /* the reading thread, resolved to a name only at dump time (keeps the hot path cheap) */
} asset_rec;
static asset_rec g_asset_ring[TL_ASSET_RING];
static unsigned long g_asset_writes;    /* how many reads recorded, ever (accessed only through __atomic_*) */
static int g_asset_forced = -1;
void tl_asset_read_note(const char *what, int ok, int err)
{
    if (g_asset_forced < 0) g_asset_forced = getenv("TL_ASSET_TRACE") ? 1 : 0;
    const char *name = (what && what[0]) ? what : "(unnamed entry)";
    unsigned long w = __atomic_fetch_add(&g_asset_writes, 1, __ATOMIC_RELAXED) + 1;
    asset_rec *r = &g_asset_ring[(w - 1) % TL_ASSET_RING];
    r->ok = ok; r->err = err;
    snprintf(r->name, sizeof(r->name), "%s", name);
    r->tid = (unsigned long)(uintptr_t)pthread_self();
    __atomic_store_n(&r->seq, w, __ATOMIC_RELEASE);
    if (g_asset_forced) {
        char me[32] = "?"; pthread_getname_np(pthread_self(), me, sizeof(me));
        if (ok < 0)      tl_log_line("asset: opening '%s' by '%s' (the app's first-frame path)", name, me);
        else if (ok)     tl_log_line("asset: read '%s' served by '%s'", name, me);
        else             tl_log_line("asset: read of '%s' FAILED (errno %d) by '%s'", name, err, me);
    }
}
void tl_asset_ring_dump(const char *tag)
{
    unsigned long n = __atomic_load_n(&g_asset_writes, __ATOMIC_RELAXED);
    if (!n) return;
    unsigned long cnt = n < TL_ASSET_RING ? n : TL_ASSET_RING;
    unsigned long first = n - cnt + 1;
    tl_log_line("asset[%s]: the %lu asset read(s) recorded before this point, oldest first (#%lu..#%lu of %lu this session) -- these are the app's OWN last reads, so the freeze happened after (or inside) the last one:",
                tag, cnt, first, n, n);
    for (unsigned long w = first; w <= n; w++) {
        asset_rec *r = &g_asset_ring[(w - 1) % TL_ASSET_RING];
        if (__atomic_load_n(&r->seq, __ATOMIC_ACQUIRE) != w) continue;      /* overwritten while we read: skip, never a torn name */
        char me[32] = "?"; pthread_getname_np((pthread_t)(uintptr_t)r->tid, me, sizeof(me));
        if (r->ok < 0)      tl_log_line("asset[%s]: #%lu opening '%s' by '%s'", tag, w, r->name, me);
        else if (r->ok)     tl_log_line("asset[%s]: #%lu read '%s' served by '%s'", tag, w, r->name, me);
        else                tl_log_line("asset[%s]: #%lu read of '%s' FAILED (errno %d) by '%s'", tag, w, r->name, r->err, me);
    }
}
void tl_asset_ring_reset(void)
{
    for (int i = 0; i < TL_ASSET_RING; i++) __atomic_store_n(&g_asset_ring[i].seq, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_asset_writes, 0, __ATOMIC_RELAXED);
}

/* One line per distinct entry: that an APK entry was resolved (name, size, how served), and that one asked for is not in
 * the APK -- an honest ENOENT, said once. */
static void jar_note(const jar_entry *j, bool found)
{
    static char said[64][192];
    static int nsaid;
    const char *key = j->entry[0] ? j->entry : j->apk;
    for (int i = 0; i < nsaid; i++) if (!strcmp(said[i], key)) return;
    if (nsaid < 64) snprintf(said[nsaid++], sizeof(said[0]), "%s", key);
    if (found) {
        static int said_first;
        if (!said_first++) tl_log_line("husk: first content read this session: %s", j->entry[0] ? j->entry : j->apk);
        tl_log_line("husk: apk entry %s: %llu bytes, %s, from %s", j->entry, (unsigned long long)j->size,
                    j->method == 0 ? "stored (read in place)" : "deflated (inflated once, then cached under .jar-cache)", j->apk);
    }
    else
        tl_log_line("husk: apk entry %s: not in %s (ENOENT)", j->entry[0] ? j->entry : "(no entry named)", j->apk[0] ? j->apk : "(the APK)");
}

/* The other half of the line above, for when an entry is *not* served: the "served" line only prints once the descriptor
 * and its redirection are both in place. Says whether the APK open failed (with its errno), or the descriptor number was
 * outside the redirection table (fd >= 4096, where a read would come from the APK's own bytes at its own offset while
 * fstat() reported the APK's size -- a wrong file, not an error). */
static void jar_note_serve_failed(const jar_entry *j, int fd, int e)
{
    static char said[64][192];
    static int nsaid;
    const char *key = j->entry[0] ? j->entry : j->apk;
    for (int i = 0; i < nsaid; i++) if (!strcmp(said[i], key)) return;
    if (nsaid < 64) snprintf(said[nsaid++], sizeof(said[0]), "%s", key);
    if (fd < 0)
        tl_log_line("husk: apk entry %s: resolved (%llu bytes, %s) but its %s could not be opened (errno %d), so the open fails",
                    key, (unsigned long long)j->size, j->method == 0 ? "stored" : "deflated",
                    j->method == 0 ? "APK" : "inflated cache file", e);
    else
        tl_log_line("husk: apk entry %s: resolved (%llu bytes, %s) but its descriptor %d is outside the redirection table, so the open fails",
                    key, (unsigned long long)j->size, j->method == 0 ? "stored" : "deflated", fd);
    tl_asset_read_note(key, 0, fd < 0 ? e : 24);
}

/*
 * A compressed entry, inflated once into <data>/.jar-cache: the path to open instead, or
 * NULL. The cache file names the entry, its uncompressed AND compressed size, and a
 * fingerprint of the APK it came from (path, size, mtime) -- entry identity, size, and the
 * app's own version. A rebuilt or updated APK yields a different name, so a stale inflated
 * copy is never served; the size check below is the last guard against a torn file. Written
 * under a .part name and renamed into place, so a launch that dies mid-write cannot leave a
 * torn file behind (and drop_unfinished sweeps any .husk-new, not this).
 */
static const char *jar_inflated(const jar_entry *j, char *buf, size_t n)
{
    char name[512]; size_t k = 0;
    for (const char *c = j->entry; *c && k + 1 < sizeof(name); c++) name[k++] = (*c == '/') ? '_' : *c;
    name[k] = 0;
    char dir[1100];
    snprintf(dir, sizeof(dir), "%s/.jar-cache", tl_data_dir());
    mkdir(dir, 0755);
    uint64_t fp = load_entry_identity(j->apk, j->entry, j->size, j->csize);
    snprintf(buf, n, "%s/%016llx_%s", dir, (unsigned long long)fp, name);
    struct stat st;
    if (stat(buf, &st) == 0 && (uint64_t)st.st_size == j->size) {
        tl_load_add(TL_LOAD_COPY, 0, j->size);          /* served from the inflated copy: no inflate */
        return buf;
    }
    pthread_mutex_lock(&g_jar_lock);
    const tl_zip_entry *e = tl_zip_find(&g_jar_zip, j->entry);
    const uint8_t *data = NULL; size_t len = 0; bool owned = false; char err[160];
    unsigned long long t0 = tl_load_now_ns();
    bool ok = e && tl_zip_data(&g_jar_zip, e, 1u << 30, &data, &len, &owned, err, sizeof(err));
    if (ok) tl_load_add(TL_LOAD_INFLATE, tl_load_now_ns() - t0, len);
    pthread_mutex_unlock(&g_jar_lock);
    if (!ok) return NULL;
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s.part", buf);
    FILE *f = fopen(tmp, "wb");
    bool wrote = f && fwrite(data, 1, len, f) == len;
    if (f) fclose(f);
    if (owned) free((void *)data);
    if (!wrote || rename(tmp, buf) != 0) { unlink(tmp); return NULL; }
    return buf;
}

/* ----------------------------------------------------------- open & friends */

/* Linux arm64 open flags -> Darwin. */
static int oflags_to_darwin(int f)
{
    int d = f & 3;                                       /* O_RDONLY / O_WRONLY / O_RDWR agree */
    if (f & 0x40)      d |= O_CREAT;
    if (f & 0x80)      d |= O_EXCL;
    if (f & 0x100)     d |= O_NOCTTY;
    if (f & 0x200)     d |= O_TRUNC;
    if (f & 0x400)     d |= O_APPEND;
    if (f & 0x800)     d |= O_NONBLOCK;
    if (f & 0x101000)  d |= O_SYNC;
    if (f & 0x4000)    d |= O_DIRECTORY;
    if (f & 0x8000)    d |= O_NOFOLLOW;
    if (f & 0x80000)   d |= O_CLOEXEC;
    return d;
}
static int oflags_from_darwin(int d)
{
    int f = d & 3;
    if (d & O_APPEND) f |= 0x400;
    if (d & O_NONBLOCK) f |= 0x800;
    return f;
}

/* --------------------------------------------------------- crash-safe saves */

/*
 * A game rewrites a save by opening it with O_TRUNC and writing it again, so for a moment the file is empty; iOS can end an
 * app at any moment, leaving an empty save (Minecraft Dungeons then waits forever on one it cannot read). So a file that
 * already has contents and lives in the game's own data is not truncated: the game writes a new file beside it, which takes
 * the old one's name when the game closes it. Ended halfway, the old save is still there, whole.
 */
static char *g_atomic[4096];                 /* fd -> the path the new file becomes on close */
static pthread_mutex_t g_atomic_lock = PTHREAD_MUTEX_INITIALIZER;

int tl_atomic_open(const char *real, int dflags, unsigned mode)
{
    if (!(dflags & O_TRUNC) || (dflags & O_ACCMODE) == O_RDONLY || (dflags & (O_APPEND | O_EXCL))) return -2;
    const char *data = tl_data_dir();
    size_t dn = strlen(data);
    if (strncmp(real, data, dn) || real[dn] != '/') return -2;
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) return -2;
    char tmp[1100];
    if (snprintf(tmp, sizeof(tmp), "%s.husk-new", real) >= (int)sizeof(tmp)) return -2;
    (void)mode;
    int fd = open(tmp, (dflags & ~O_EXCL) | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (fd < 0) return -2;                   /* the plain way, then */
    if (fd >= 4096) { close(fd); unlink(tmp); return -2; }
    pthread_mutex_lock(&g_atomic_lock);
    free(g_atomic[fd]);
    g_atomic[fd] = strdup(real);
    pthread_mutex_unlock(&g_atomic_lock);
    return fd;
}

/* A new file that will not be finished (it could not be opened as a stream): drop it, and the old one stays as it was. */
void tl_atomic_abandon(int fd)
{
    if (fd < 0 || fd >= 4096) return;
    pthread_mutex_lock(&g_atomic_lock);
    char *real = g_atomic[fd];
    g_atomic[fd] = NULL;
    pthread_mutex_unlock(&g_atomic_lock);
    if (!real) return;
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.husk-new", real);
    unlink(tmp);
    free(real);
}

/* After fd was closed: give a new file the name of the one it replaces. */
void tl_atomic_closed(int fd)
{
    if (fd < 0 || fd >= 4096) return;
    pthread_mutex_lock(&g_atomic_lock);
    char *real = g_atomic[fd];
    g_atomic[fd] = NULL;
    pthread_mutex_unlock(&g_atomic_lock);
    if (!real) return;
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.husk-new", real);
    if (rename(tmp, real) != 0) tl_log_line("file: could not put the new %s in place (%s)", real, strerror(errno));
    free(real);
}

/* A game's expansion file that is not there: said once (the store installs APKs and splits, not expansion files). The path
 * printed is the one the file system looked in, so the line says where the OBB has to go as well as that it is missing. */
void tl_note_missing_file(const char *path)
{
    static bool said;
    if (said || !path) return;
    const char *dot = strrchr(path, '.');
    if (strstr(path, "/obb/") && dot && !strcmp(dot, ".obb")) { said = true; tl_log_line("husk: the app's expansion file (OBB) is missing: %s", path); }
}
/* The native library directory holds empty stand-ins for the APK's libraries; one opened to be read gets the library's real
 * bytes first (host path). Not on a plain stat: Unity looks at libil2cpp's (100 MB) without reading it. */
static void fill_lib_standin(const char *real)
{
    size_t dl = strlen(tl_data_dir()); const char *nm = strrchr(real, '/');
    struct stat lst;
    if (nm && !strncmp(real, tl_data_dir(), dl) && !strncmp(real + dl, "/lib/", 5) && real + dl + 4 == nm && strlen(nm) > 4
        && !strcmp(nm + strlen(nm) - 3, ".so") && stat(real, &lst) == 0 && lst.st_size == 0) {
        bool tl_ld_has_lib(const char *name); bool tl_ld_extract_lib(const char *name, const char *dest);
        if (tl_ld_has_lib(nm + 1)) tl_ld_extract_lib(nm + 1, real);
    }
}
/* The same for a name relative to a directory descriptor. */
static void fill_lib_standin_at(int dirfd, const char *name)
{
    char dir[PATH_MAX], full[PATH_MAX + 256];
    if (!name || name[0] == '/' || fcntl(dirfd, F_GETPATH, dir) != 0) return;
    snprintf(full, sizeof(full), "%s/%s", dir, name);
    fill_lib_standin(full);
}
/* The Java runtime's own data is in ordinary FILES the layer was handed (the i18n and tzdata APEXes' icudt*.dat and tz/),
 * read by ICU4J through android.system.Os.open -- arriving here as a plain open() of an ordinary path. When such a path
 * will not open, the Java code dies inside a static initialiser with a bare NullPointerException, so the path and its errno
 * are said once here. */
static bool runtime_data_path(const char *p)
{
    return p && (strstr(p, "/usr/icu/") || strstr(p, "/etc/icu/") || strstr(p, "/icu/icudt") ||
                 strstr(p, "/tz/versioned/") || strstr(p, "/usr/share/zoneinfo/"));
}
static void tl_note_missing_data(const char *path, int e)
{
    static char said[16][192];
    static int nsaid;
    if (!runtime_data_path(path)) return;
    for (int i = 0; i < nsaid; i++) if (!strcmp(said[i], path)) return;
    if (nsaid < 16) snprintf(said[nsaid++], sizeof(said[0]), "%s", path);
    tl_log_line("husk: the runtime's own data file could not be opened: %s (errno %d); the Java code that wanted it fails in a static initialiser with a bare NullPointerException", path, e);
}

/*
 * The paths a failed open() looked in, in the order first seen -- always armed, bounded, once per distinct path. libcore's
 * own native code reports an ENOENT as a bare IOException ("No such file or directory") with NO path, so a boot that dies
 * inside libcore's natives says only the errno (this run: "dvm: loading libcore's natives (libopenjdk.so) threw
 * java.io.IOException: No such file or directory"). The DVM's natives loader dumps this table when such an exception is
 * pending, so the next log names the FILE (or directory) that was missing. Only failed opens are recorded, so a successful
 * open pays nothing; recording a failure is a bounded string copy plus a short dedupe scan, off the hot path by definition.
 */
#define TL_OPEN_FAIL_MAX 32
static struct { char path[192]; int err; } g_open_fail[TL_OPEN_FAIL_MAX];
static int g_open_fail_n;                       /* how many DISTINCT failing paths were seen, ever (kept up to the cap) */
static void tl_open_fail_note(const char *path, int err)
{
    if (!path || !path[0]) return;
    int kept = g_open_fail_n < TL_OPEN_FAIL_MAX ? g_open_fail_n : TL_OPEN_FAIL_MAX;
    for (int i = 0; i < kept; i++) if (!strcmp(g_open_fail[i].path, path)) return;      /* once per distinct path */
    if (g_open_fail_n < TL_OPEN_FAIL_MAX) {
        snprintf(g_open_fail[g_open_fail_n].path, sizeof(g_open_fail[0].path), "%s", path);
        g_open_fail[g_open_fail_n].err = err;
    }
    g_open_fail_n++;                            /* still counted past the cap, so the dump can say how many more */
}
void tl_open_fail_dump(const char *why)
{
    int n = g_open_fail_n < TL_OPEN_FAIL_MAX ? g_open_fail_n : TL_OPEN_FAIL_MAX;
    if (!n) return;
    tl_log_line("open[%s]: the %d path(s) a failed open() looked in, in the order first seen -- the file the guest reports only as \"No such file or directory\" is one of these:", why, n);
    for (int i = 0; i < n; i++)
        tl_log_line("open[%s]: '%s' (errno %d)", why, g_open_fail[i].path, g_open_fail[i].err);
    if (g_open_fail_n > TL_OPEN_FAIL_MAX)
        tl_log_line("open[%s]: (%d more distinct path(s) failed but were not kept: the table is bounded)", why, g_open_fail_n - TL_OPEN_FAIL_MAX);
}
static int b_open(const char *path, int flags, unsigned mode)
{
    tl_hp_bump(TL_HP_OPEN);
    char buf[1024]; static __thread char content[32768];
    jar_entry jar;
    if (renpy_hidden_macver(path)) { tl_set_guest_errno(2); return -1; }   /* Ren'Py must not look like a Mac (see above) */
    if (jar_lookup(path, &jar)) {
        /* The paired instrument: what the swapped-in app is reading (bounded, and only after a swap). */
        tl_asset_read_note(jar.entry[0] ? jar.entry : path, -1, 0);
        if (!jar.found) jar_note(&jar, false);
        if (!jar.found || jar.dir || (flags & 3) != 0) { tl_asset_read_note(jar.entry[0] ? jar.entry : path, 0, jar.dir ? 21 : 2); tl_set_guest_errno(jar.dir ? 21 : 2); ftrace_open_jar(path, -1); return -1; }
        int fd = -1, e = 0;
        if (jar.method == 0) {
            /* A stored entry is a stretch of the APK, so the APK is opened and every read of that descriptor is
             * redirected into the entry (g_vfd) -- the redirect *is* the entry. Refused here instead of silently: an APK
             * that will not open, and a descriptor number outside the table (a read would come from the APK's own offset
             * while fstat() reported the APK's size -- the wrong file with no error). */
            TL_ERRNO_BEGIN(); fd = open(jar.apk, O_RDONLY); e = errno; TL_ERRNO_END();
            if (fd < 0 || fd >= 4096) {
                jar_note_serve_failed(&jar, fd, e);
                if (fd >= 0) close(fd);
                tl_set_guest_errno(fd < 0 ? (e ? tl_errno_to_guest(e) : 5 /* EIO */) : 24 /* EMFILE */);
                ftrace_open_jar(path, -1);
                return -1;
            }
            vfd_set(fd, jar.off, jar.size, jar.entry);
        } else {
            char inflated[1200];
            const char *real = jar_inflated(&jar, inflated, sizeof(inflated));
            TL_ERRNO_BEGIN(); fd = real ? open(real, O_RDONLY) : -1; e = errno; TL_ERRNO_END();
            if (fd < 0) {
                jar_note_serve_failed(&jar, fd, e);
                tl_set_guest_errno(e ? tl_errno_to_guest(e) : 5 /* EIO */);
                ftrace_open_jar(path, -1);
                return -1;
            }
            vfd_take_over(fd);        /* a plain inflated copy on disk, not the entry: a fresh number must carry nothing of one */
        }
        jar_note(&jar, true);              /* said once the entry is actually served, never before (see jar_note_serve_failed) */
        ftrace_open_jar(path, fd);
        return fd;
    }
    const char *real = tl_path_resolve(path, buf, sizeof(buf));
    if (real && (flags & 3) == 0) fill_lib_standin(real);
    if (strstr(path, "/auxv")) { int afd = tl_synth_open(path); if (afd >= 0) { vfd_take_over(afd); return afd; } }   /* binary, not text */
    const char *s = synth_content(path, content, sizeof(content));
    if (s) {
        int fd = synth_file(s);
        if (fd < 0) tl_set_guest_errno(2); else vfd_take_over(fd);
        return fd;
    }
    const vfile *vf = (flags & 3) == 0 ? vfile_find(path) : NULL;
    if (vf) {
        TL_ERRNO_BEGIN();
        int fd = open(vf->host, O_RDONLY);
        int e = errno;
        TL_ERRNO_END();
        if (fd >= 0 && fd < 4096) vfd_set(fd, vf->off, vf->size, vf->name);
        { static int tr = -1, said; if (tr < 0) tr = getenv("TL_FILE_TRACE") ? atoi(getenv("TL_FILE_TRACE")) : 0; if (tr && said++ < (tr > 1 ? 4000000 : 20000)) tl_log_line("file: open(%s) -> %d (virtual: %llu bytes at %llu of %s)%s", path, fd, (unsigned long long)vf->size, (unsigned long long)vf->off, vf->host, fd < 0 ? (e == ENOENT ? " ENOENT" : " error") : ""); }
        return fd;
    }
    TL_ERRNO_BEGIN();
    int fd = tl_atomic_open(real, oflags_to_darwin(flags), mode);
    if (fd == -2) fd = open(real, oflags_to_darwin(flags), mode);
    int e = errno;
    TL_ERRNO_END();
    { static int tr = -1, said; if (tr < 0) tr = getenv("TL_FILE_TRACE") ? atoi(getenv("TL_FILE_TRACE")) : 0; if (tr && said++ < (tr > 1 ? 4000000 : 20000)) tl_log_line("file: open(%s, %#x) -> %d%s", path, flags, fd, fd < 0 ? (e == ENOENT ? " ENOENT" : " error") : ""); }
    if (fd >= 0) vfd_take_over(fd);   /* an ordinary file opened on this number: no redirect of an older file may survive onto it */
    if (fd < 0 && e == ENOENT) { tl_open_fail_note(real, e); tl_note_missing_file(real); tl_note_missing_data(real, e); }   /* name the host path it looked in, not just the guest one */
    return fd;
}
static int b___open_2(const char *path, int flags) { return b_open(path, flags, 0); }
static bool net_trace_fd(int fd);
/* eventfds are pipes whose read end is the descriptor the guest holds: writes to it go to the other end, and closing it closes both */
static int g_evfd_peer[1024];
void tl_eventfd_register(int rd, int wr) { if (rd >= 0 && rd < 1024) g_evfd_peer[rd] = wr + 1; }
static int evfd_peer(int fd) { return fd >= 0 && fd < 1024 ? g_evfd_peer[fd] - 1 : -1; }
static int b_close(int fd) { if (evfd_peer(fd) >= 0) { close(evfd_peer(fd)); g_evfd_peer[fd] = 0; } vfd_drop(fd); bool sock = net_trace_fd(fd); TL_ERRNO_BEGIN(); int r = close(fd); tl_atomic_closed(fd); TL_ERRNO_END(); if (sock) tl_log_line("net: close(fd %d)", fd); return r; }
/* close() as the guest calls it (for other shims that close on its behalf: fdsan's close_with_tag) */
int tl_guest_close(int fd) { return b_close(fd); }
static bool net_trace_fd(int fd)
{
    static int on = -1;
    if (on < 0) on = getenv("TL_NET_TRACE") ? 1 : 0;
    if (!on) return false;
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode);
}
static long b_read(int fd, void *p, size_t n)
{
    tl_hp_bump(TL_HP_READ);
    if (vfd_is(fd)) {
        vfd *v = &g_vfd[fd];
        if (v->pos >= v->size) return 0;
        if (n > v->size - v->pos) n = (size_t)(v->size - v->pos);
        uint64_t pos = v->pos;
        { static int tr = -1, said; if (tr < 0) tr = getenv("TL_FILE_TRACE") ? atoi(getenv("TL_FILE_TRACE")) : 0; if (tr && said++ < (tr > 1 ? 4000000 : 400)) tl_log_line("file: read(fd %d, %zu bytes at %llu of the virtual file)", fd, n, (unsigned long long)pos); }
        TL_ERRNO_BEGIN(); long r = pread(fd, kernel_dst(p, n), n, (off_t)(v->off + pos)); int e = errno; TL_ERRNO_END();
        if (r < 0) { vfd_note_failed(fd, "read", v, pos, n, e); vfd_note_dst(true, fd, "read", p, n, e); }
        if (r > 0) v->pos += (uint64_t)r;
        return r;
    }
    TL_ERRNO_BEGIN(); long r = read(fd, kernel_dst(p, n), n); int e = errno; TL_ERRNO_END();
    if (r < 0) vfd_note_dst(false, fd, "read", p, n, e);
    if (fd >= 0 && fd < 4096 && g_vfd[fd].was) vfd_note_dropped(fd, "read", r, e);     /* the layer DID redirect this number once (see vfd_drop) */
    { static int tr = -1; if (tr < 0) tr = getenv("TL_TIMERFD_TRACE") ? 1 : 0;
      bool tl_timerfd_is(int fd);
      if (tr && tl_timerfd_is(fd)) { char nm[32] = ""; pthread_getname_np(pthread_self(), nm, sizeof(nm)); tl_log_line("timerfd[%s]: read fd %d -> %ld errno %d", nm, fd, r, r < 0 ? e : 0); } }
    if (net_trace_fd(fd)) tl_log_line("net: read(fd %d, %zu) -> %ld errno %d", fd, n, r, r < 0 ? e : 0);
    { static int tr = -1, said; if (tr < 0) tr = getenv("TL_FILE_TRACE") ? atoi(getenv("TL_FILE_TRACE")) : 0; if (tr && n == 32 && said++ < 20) tl_log_line("file: read(fd %d, 32) -> %ld errno %d", fd, r, r < 0 ? e : 0); }
    return r;
}
static long b___read_chk(int fd, void *p, size_t n, size_t bufsz)
{
    if (n > bufsz) { tl_log_line("bionic: __read_chk overflow"); abort(); }
    return b_read(fd, p, n);
}
static long b_write(int fd, const void *p, size_t n)
{
    if (evfd_peer(fd) >= 0) fd = evfd_peer(fd);
    TL_ERRNO_BEGIN(); long r = write(fd, p, n); int e = errno; TL_ERRNO_END();
    if (net_trace_fd(fd)) tl_log_line("net: write(fd %d, %zu) -> %ld errno %d", fd, n, r, r < 0 ? e : 0);
    return r;
}
/* eventfd's helpers, over the pipe an eventfd is here */
static int b_eventfd_write(int fd, uint64_t v) { return b_write(fd, &v, sizeof(v)) == (long)sizeof(v) ? 0 : -1; }
static int b_eventfd_read(int fd, uint64_t *v) { return b_read(fd, v, sizeof(*v)) == (long)sizeof(*v) ? 0 : -1; }
/* mount tables (libopenjdk's FileStore): there are none to list */
static void *b_setmntent(const char *path, const char *mode) { (void)path; (void)mode; tl_set_guest_errno(2); return NULL; }
static void *b_getmntent(void *f) { (void)f; return NULL; }
static void *b_getmntent_r(void *f, void *ent, char *buf, int n) { (void)f; (void)ent; (void)buf; (void)n; return NULL; }
static int b_endmntent(void *f) { (void)f; return 1; }
/* write() with the buffer's size checked first, as bionic's FORTIFY does */
static long b___write_chk(int fd, const void *p, size_t n, size_t bufsize) { if (n > bufsize) abort(); return b_write(fd, p, n); }
static long b_writev(int fd, const struct iovec *v, int n) { TL_ERRNO_BEGIN(); long r = writev(fd, v, n); TL_ERRNO_END(); return r; }
static long b_pread64(int fd, void *p, size_t n, long off);   /* b_preadv (below) serves its iovecs through it */
/* readv/preadv on a descriptor redirecting into an APK entry: the redirect must serve these exactly as it serves read, or
 * the guest is handed the APK's own bytes -- the wrong file, silently. (readv used to be a plain passthrough and preadv was
 * not wrapped, so how the guest spelled the call decided the answer.) Each iovec is served in order; a short iovec ends
 * the call, as readv would. */
static long b_readv(int fd, const struct iovec *v, int n)
{
    if (vfd_is(fd)) {
        long total = 0;
        for (int i = 0; i < n; i++) {
            if (!v[i].iov_len) continue;
            long r = b_read(fd, v[i].iov_base, v[i].iov_len);
            if (r < 0) return total > 0 ? total : r;
            total += r;
            if ((size_t)r < v[i].iov_len) break;
        }
        return total;
    }
    struct iovec iv[32];
    if (iov_dst(v, n, iv, 32)) { TL_ERRNO_BEGIN(); long r = readv(fd, iv, n); TL_ERRNO_END(); return r; }
    TL_ERRNO_BEGIN(); long r = readv(fd, v, n); TL_ERRNO_END(); return r;
}
static long b_preadv(int fd, const struct iovec *v, int n, long off)
{
    if (vfd_is(fd)) {
        long total = 0;
        for (int i = 0; i < n; i++) {
            if (!v[i].iov_len) continue;
            long r = b_pread64(fd, v[i].iov_base, v[i].iov_len, off + total);
            if (r < 0) return total > 0 ? total : r;
            total += r;
            if ((size_t)r < v[i].iov_len) break;
        }
        return total;
    }
    struct iovec iv[32];
    if (iov_dst(v, n, iv, 32)) { TL_ERRNO_BEGIN(); long r = preadv(fd, iv, n, off); TL_ERRNO_END(); return r; }
    TL_ERRNO_BEGIN(); long r = preadv(fd, v, n, off); TL_ERRNO_END(); return r;
}
static long b_pread64(int fd, void *p, size_t n, long off)
{
    tl_hp_bump(TL_HP_PREAD64);
    if (vfd_is(fd)) {
        const vfd *v = &g_vfd[fd];
        if (off < 0) { tl_set_guest_errno(22); return -1; }
        if ((uint64_t)off >= v->size) return 0;
        if (n > v->size - (uint64_t)off) n = (size_t)(v->size - (uint64_t)off);
        { static int tr = -1, said; if (tr < 0) tr = getenv("TL_FILE_TRACE") ? atoi(getenv("TL_FILE_TRACE")) : 0; if (tr && said++ < (tr > 1 ? 4000000 : 400)) tl_log_line("file: pread(fd %d, %zu bytes at %ld of the virtual file)", fd, n, off); }
        TL_ERRNO_BEGIN(); long r = pread(fd, kernel_dst(p, n), n, off + (long)v->off); int e = errno; TL_ERRNO_END();
        if (r < 0) { vfd_note_failed(fd, "pread64", v, (uint64_t)off, n, e); vfd_note_dst(true, fd, "pread64", p, n, e); }
        return r;
    }
    TL_ERRNO_BEGIN(); long r = pread(fd, kernel_dst(p, n), n, off); int e = errno; TL_ERRNO_END();
    if (r < 0) vfd_note_dst(false, fd, "pread64", p, n, e);
    if (fd >= 0 && fd < 4096 && g_vfd[fd].was) vfd_note_dropped(fd, "pread64", r, e);
    return r;
}
static long b_pwrite64(int fd, const void *p, size_t n, long off) { TL_ERRNO_BEGIN(); long r = pwrite(fd, p, n, off); TL_ERRNO_END(); return r; }
static long b___pwrite64_chk(int fd, const void *p, size_t n, long off, size_t bufsz)
{
    if (n > bufsz) { tl_log_line("bionic: __pwrite64_chk overflow"); abort(); }
    return b_pwrite64(fd, p, n, off);
}
static long b___pread64_chk(int fd, void *p, size_t n, long off, size_t bufsz)
{
    if (n > bufsz) { tl_log_line("bionic: __pread64_chk overflow"); abort(); }
    return b_pread64(fd, p, n, off);
}
static long b_lseek(int fd, long off, int whence)
{
    if (vfd_is(fd)) {
        vfd *v = &g_vfd[fd];
        int64_t np = whence == 0 ? off : whence == 1 ? (int64_t)v->pos + off : (int64_t)v->size + off;
        if (np < 0) { tl_set_guest_errno(22); return -1; }
        v->pos = (uint64_t)np;
        return np;
    }
    TL_ERRNO_BEGIN(); long r = lseek(fd, off, whence); int e = errno; TL_ERRNO_END();
    if (r < 0 && fd >= 0 && fd < 4096 && g_vfd[fd].was) vfd_note_dropped(fd, "lseek", r, e);
    return r;
}
static int b_dup(int fd) { TL_ERRNO_BEGIN(); int r = dup(fd); TL_ERRNO_END(); if (r >= 0) vfd_copy(r, fd); return r; }
/* The host's stdin/stdout/stderr stay the host's: apps that send them to logcat dup2 a pipe over them, which would swallow
 * Husk's own log. The redirect is reported done; their pipe stays empty. */
static int b_dup2(int a, int b) { if (b >= 0 && b <= 2 && a != b) return b; TL_ERRNO_BEGIN(); int r = dup2(a, b); if (r >= 0 && a != b) { tl_atomic_closed(b); vfd_copy(b, a); } TL_ERRNO_END(); return r; }
static int b_pipe(int fds[2]) { TL_ERRNO_BEGIN(); int r = pipe(fds); TL_ERRNO_END(); return r; }
static int b_fsync(int fd) { TL_ERRNO_BEGIN(); int r = fsync(fd); TL_ERRNO_END(); return r; }
static int b_ftruncate(int fd, long n) { TL_ERRNO_BEGIN(); int r = ftruncate(fd, n); TL_ERRNO_END(); return r; }
static int b_truncate(const char *p, long n) { char b[1024]; TL_ERRNO_BEGIN(); int r = truncate(tl_path_resolve(p, b, sizeof(b)), n); TL_ERRNO_END(); return r; }
static int b_isatty(int fd) { int r = isatty(fd); if (!r) tl_set_guest_errno(25); return r; }
static int b_flock(int fd, int op) { TL_ERRNO_BEGIN(); int r = flock(fd, op); TL_ERRNO_END(); return r; }

#define PATH1(rt, name, call) \
    static rt b_##name(const char *p) { char b[1024]; TL_ERRNO_BEGIN(); rt r = call(tl_path_resolve(p, b, sizeof(b))); TL_ERRNO_END(); return r; }
PATH1(int, unlink, unlink)
PATH1(int, rmdir, rmdir)
static int b_mkdir(const char *p, unsigned mode) { char b[1024]; TL_ERRNO_BEGIN(); int r = mkdir(tl_path_resolve(p, b, sizeof(b)), (mode_t)mode); TL_ERRNO_END(); return r; }
/* FIFOs (Realm signals between its threads through one in its .realm.temp folder); other device nodes are refused, as for an app */
static int b_mkfifo(const char *p, unsigned mode) { char b[1024]; TL_ERRNO_BEGIN(); int r = mkfifo(tl_path_resolve(p, b, sizeof(b)), (mode_t)(mode & 0777)); TL_ERRNO_END(); return r; }
static int b_mknod(const char *p, unsigned mode, unsigned long dev)
{
    (void)dev;
    if ((mode & 0170000) == 0010000) return b_mkfifo(p, mode);
    tl_set_guest_errno(1 /* EPERM */); return -1;
}
static int b_mknodat(int dirfd, const char *p, unsigned mode, unsigned long dev)
{
    if (dirfd != -100 /* AT_FDCWD */ && p && p[0] != '/') { tl_set_guest_errno(95 /* EOPNOTSUPP */); return -1; }
    return b_mknod(p, mode, dev);
}
static int b_mkfifoat(int dirfd, const char *p, unsigned mode) { return b_mknodat(dirfd, p, (mode & 0777) | 0010000, 0); }
/* TL_FILE_TRACE also says what is asked of the file system without opening anything: which paths a game probes for, and whether they are there. */
static void ftrace(const char *what, const char *path, int r, int e)
{
    static int tr = -1, said;
    if (tr < 0) tr = getenv("TL_FILE_TRACE") ? atoi(getenv("TL_FILE_TRACE")) : 0;
    if (tr && said++ < (tr > 1 ? 4000000 : 3000)) tl_log_line("file: %s(%s) -> %d%s", what, path ? path : "(null)", r, r < 0 ? (e == ENOENT ? " ENOENT" : " error") : "");
}
static int b_access(const char *p, int m) { char b[1024]; jar_entry jar; if (renpy_hidden_macver(p)) { tl_set_guest_errno(2); return -1; } if (synth_dir(p)) return 0; if (jar_lookup(p, &jar)) { if (!jar.found) { tl_set_guest_errno(2); return -1; } if (m & 2) { tl_set_guest_errno(13); return -1; } return 0; } if (vfile_find(p)) { if (m & 2) { tl_set_guest_errno(13); return -1; } return 0; } TL_ERRNO_BEGIN(); int r = access(tl_path_resolve(p, b, sizeof(b)), m); int e = errno; TL_ERRNO_END(); ftrace("access", p, r, e); return r; }
static int b_chmod(const char *p, unsigned m) { char b[1024]; TL_ERRNO_BEGIN(); int r = chmod(tl_path_resolve(p, b, sizeof(b)), (mode_t)m); TL_ERRNO_END(); return r; }
static int b_fchmod(int fd, unsigned m) { TL_ERRNO_BEGIN(); int r = fchmod(fd, (mode_t)m); TL_ERRNO_END(); return r; }
static int b_link(const char *a, const char *b2) { char x[1024], y[1024]; TL_ERRNO_BEGIN(); int r = link(tl_path_resolve(a, x, sizeof(x)), tl_path_resolve(b2, y, sizeof(y))); TL_ERRNO_END(); return r; }
static int b_symlink(const char *a, const char *b2) { char y[1024]; TL_ERRNO_BEGIN(); int r = symlink(a, tl_path_resolve(b2, y, sizeof(y))); TL_ERRNO_END(); return r; }
static bool is_proc_exe(const char *p) { return p && (!strcmp(p, "/proc/self/exe") || (!strncmp(p, "/proc/", 6) && strstr(p, "/exe") && strlen(strstr(p, "/exe")) == 4)); }
static long b_readlink(const char *p, char *buf, size_t n)
{
    /* An Android app's executable is the zygote's app_process; the host has no /proc, and code that sizes a string with
     * the result (DXVK's exe-name lookup) throws when it gets -1. */
    if (is_proc_exe(p)) {
        static const char exe[] = "/system/bin/app_process64";
        size_t l = strlen(exe) < n ? strlen(exe) : n;
        memcpy(buf, exe, l);
        return (long)l;
    }
    char b[1024]; TL_ERRNO_BEGIN(); long r = readlink(tl_path_resolve(p, b, sizeof(b)), buf, n); TL_ERRNO_END(); return r;
}
static char *b_realpath(const char *p, char *out) { char b[1024]; TL_ERRNO_BEGIN(); char *r = realpath(tl_path_resolve(p, b, sizeof(b)), out); TL_ERRNO_END(); return r; }
static char *b_getcwd(char *buf, size_t n) { TL_ERRNO_BEGIN(); char *r = getcwd(buf, n); TL_ERRNO_END(); return r; }
static char *b___getcwd_chk(char *buf, size_t n, size_t bl) { (void)bl; return b_getcwd(buf, n); }
static int b_utimes(const char *p, const struct timeval tv[2]) { char b[1024]; TL_ERRNO_BEGIN(); int r = utimes(tl_path_resolve(p, b, sizeof(b)), tv); TL_ERRNO_END(); return r; }
static int b_utime(const char *p, const struct utimbuf *t) { char b[1024]; TL_ERRNO_BEGIN(); int r = utime(tl_path_resolve(p, b, sizeof(b)), t); TL_ERRNO_END(); return r; }
static int b_futimens(int fd, const struct timespec ts[2]) { TL_ERRNO_BEGIN(); int r = futimens(fd, ts); TL_ERRNO_END(); return r; }
static unsigned b___umask_chk(unsigned m) { return umask((mode_t)m); }

/* sendfile: Linux's (out, in, off*, count) emulated with read/write. The source may be a descriptor redirecting into an
 * APK entry: then the entry's bytes are sent and its offset advanced (b_read/b_pread64 do the redirection), not the APK's
 * own bytes at the APK's own offset. */
static long b_sendfile(int out, int in, long *off, size_t count)
{
    char buf[16384]; long total = 0;
    while ((size_t)total < count) {
        size_t chunk = count - (size_t)total < sizeof(buf) ? count - (size_t)total : sizeof(buf);
        long n = vfd_is(in) ? (off ? b_pread64(in, buf, chunk, (long)(*off + total)) : b_read(in, buf, chunk))
                            : (off ? pread(in, buf, chunk, *off) : read(in, buf, chunk));
        if (n <= 0) break;
        long w = write(out, buf, (size_t)n);
        if (w <= 0) break;
        total += w;
        if (off) *off += w;
    }
    return total;
}

/* --- stat: bionic arm64's 128-byte layout --- */

typedef struct {
    uint64_t st_dev, st_ino; uint32_t st_mode, st_nlink, st_uid, st_gid; uint64_t st_rdev, pad1;
    int64_t st_size; int32_t st_blksize, pad2; int64_t st_blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns; uint32_t unused4, unused5;
} guest_stat;

static void fill_stat(guest_stat *g, const struct stat *s)
{
    memset(g, 0, sizeof(*g));
    g->st_dev = (uint64_t)s->st_dev; g->st_ino = s->st_ino; g->st_mode = s->st_mode; g->st_nlink = s->st_nlink;
    g->st_uid = s->st_uid; g->st_gid = s->st_gid; g->st_rdev = (uint64_t)s->st_rdev; g->st_size = s->st_size;
    g->st_blksize = s->st_blksize; g->st_blocks = s->st_blocks;
    g->atime = s->st_atimespec.tv_sec; g->atime_ns = s->st_atimespec.tv_nsec;
    g->mtime = s->st_mtimespec.tv_sec; g->mtime_ns = s->st_mtimespec.tv_nsec;
    g->ctime = s->st_ctimespec.tv_sec; g->ctime_ns = s->st_ctimespec.tv_nsec;
}
static int b_fstat(int fd, guest_stat *g);
#define fstat_guest_fd b_fstat
static int b_stat(const char *p, guest_stat *g)
{
    char b[1024], c[8192]; struct stat s;
    if (renpy_hidden_macver(p)) { tl_set_guest_errno(2); return -1; }     /* os.path.exists() must say no (see above) */
    if (synth_dir(p)) { memset(g, 0, sizeof(*g)); g->st_mode = S_IFDIR | 0555; g->st_nlink = 2; g->st_size = 4096; return 0; }
    if (synth_content(p, c, sizeof(c))) { memset(g, 0, sizeof(*g)); g->st_mode = S_IFREG | 0444; g->st_nlink = 1; return 0; }
    jar_entry jar;
    if (jar_lookup(p, &jar)) {
        ftrace("stat", p, jar.found ? 0 : -1, jar.found ? 0 : ENOENT);
        if (!jar.found || stat(jar.apk, &s) != 0) { tl_set_guest_errno(2); return -1; }
        fill_stat(g, &s);
        if (jar.dir) { g->st_mode = S_IFDIR | 0555; g->st_size = 4096; g->st_blocks = 8; }
        else { g->st_size = (int64_t)jar.size; g->st_mode = S_IFREG | 0444; g->st_blocks = (int64_t)((jar.size + 511) / 512); }
        return 0;
    }
    const vfile *vf = vfile_find(p);
    if (vf) {
        TL_ERRNO_BEGIN(); int vr = stat(vf->host, &s); int ve = errno; TL_ERRNO_END();
        ftrace("stat", p, vr, ve);
        if (vr == 0) { fill_stat(g, &s); g->st_size = (int64_t)vf->size; g->st_mode = S_IFREG | 0444; }
        return vr;
    }
    TL_ERRNO_BEGIN(); const char *real = tl_path_resolve(p, b, sizeof(b)); int r = stat(real, &s); int e = errno; TL_ERRNO_END();
    ftrace("stat", p, r, e);
    if (r != 0 && e == ENOENT) tl_note_missing_file(real);   /* name the host path it looked in, not just the guest one */
    if (r == 0) fill_stat(g, &s);
    return r;
}
static int b_lstat(const char *p, guest_stat *g)
{
    char b[1024]; struct stat s;
    if (renpy_hidden_macver(p)) { tl_set_guest_errno(2); return -1; }     /* see above */
    if (synth_dir(p)) { memset(g, 0, sizeof(*g)); g->st_mode = S_IFDIR | 0555; g->st_nlink = 2; g->st_size = 4096; return 0; }
    if (p && strstr(p, "jar:file:")) return b_stat(p, g);
    /* /proc/self/exe is a link (to app_process64, as readlink says): Dart finds its own executable through it, and its code assumes a link */
    if (is_proc_exe(p)) {
        memset(g, 0, sizeof(*g));
        g->st_mode = S_IFLNK | 0777; g->st_nlink = 1; g->st_size = 25;
        return 0;
    }
    TL_ERRNO_BEGIN(); int r = lstat(tl_path_resolve(p, b, sizeof(b)), &s); TL_ERRNO_END();
    if (r == 0) fill_stat(g, &s);
    return r;
}
/* fstatat: Android's AT_FDCWD is -100 and AT_SYMLINK_NOFOLLOW is 0x100 (macOS: -2 and 0x20). */
static int b_fstatat(int dirfd, const char *p, guest_stat *g, int flags)
{
    tl_hp_bump(TL_HP_FSTATAT);
    if (!p[0] && (flags & 0x1000 /* AT_EMPTY_PATH */)) return fstat_guest_fd(dirfd, g);
    if (p[0] == '/' || dirfd == -100) return (flags & 0x100) ? b_lstat(p, g) : b_stat(p, g);
    struct stat s;
    fill_lib_standin_at(dirfd, p);     /* .NET for Android sizes its assembly store this way before mapping it */
    TL_ERRNO_BEGIN(); int r = fstatat(dirfd, p, &s, (flags & 0x100) ? AT_SYMLINK_NOFOLLOW : 0); TL_ERRNO_END();
    if (r == 0) fill_stat(g, &s);
    return r;
}
static int b_fstat(int fd, guest_stat *g)
{
    tl_hp_bump(TL_HP_FSTAT);
    struct stat s;
    TL_ERRNO_BEGIN(); int r = fstat(fd, &s); TL_ERRNO_END();
    if (r == 0) { fill_stat(g, &s); if (vfd_is(fd)) g->st_size = (int64_t)g_vfd[fd].size; }
    return r;
}

typedef struct { int64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree; int32_t fsid[2]; int64_t f_namelen, f_frsize, f_flags, spare[4]; } guest_statfs;
static int b_statfs(const char *p, guest_statfs *g)
{
    char b[1024]; struct statfs s;
    TL_ERRNO_BEGIN(); int r = statfs(tl_path_resolve(p, b, sizeof(b)), &s); TL_ERRNO_END();
    if (r == 0) {
        memset(g, 0, sizeof(*g));
        g->f_type = 0xEF53; g->f_bsize = s.f_bsize; g->f_blocks = s.f_blocks; g->f_bfree = s.f_bfree;
        g->f_bavail = s.f_bavail; g->f_files = s.f_files; g->f_ffree = s.f_ffree; g->f_namelen = 255; g->f_frsize = s.f_bsize;
    }
    return r;
}

static int b_fstatfs(int fd, guest_statfs *g)
{
    struct statfs s;
    TL_ERRNO_BEGIN(); int r = fstatfs(fd, &s); TL_ERRNO_END();
    if (r == 0) {
        memset(g, 0, sizeof(*g));
        g->f_type = 0xEF53; g->f_bsize = s.f_bsize; g->f_blocks = s.f_blocks; g->f_bfree = s.f_bfree;
        g->f_bavail = s.f_bavail; g->f_files = s.f_files; g->f_ffree = s.f_ffree; g->f_namelen = 255; g->f_frsize = s.f_bsize;
    }
    return r;
}

/* fcntl / ioctl */
typedef struct { int16_t l_type, l_whence; int pad; int64_t l_start, l_len; int32_t l_pid; } guest_flock;

static int b_fcntl(int fd, int cmd, long arg)
{
    int r;
    TL_ERRNO_BEGIN();
    switch (cmd) {
    case 0:    r = fcntl(fd, F_DUPFD, (int)arg); if (r >= 0) vfd_copy(r, fd); break;
    case 1:    r = fcntl(fd, F_GETFD); break;
    case 2:    r = fcntl(fd, F_SETFD, (int)arg); break;
    case 3:    r = fcntl(fd, F_GETFL); if (r >= 0) r = oflags_from_darwin(r); break;
    case 4:    r = fcntl(fd, F_SETFL, oflags_to_darwin((int)arg) & ~3); break;
    case 1030: r = fcntl(fd, F_DUPFD_CLOEXEC, (int)arg); if (r >= 0) vfd_copy(r, fd); break;
    case 5: case 6: case 7: {                               /* F_GETLK, F_SETLK, F_SETLKW */
        guest_flock *g = (guest_flock *)arg;
        struct flock f = { .l_start = g->l_start, .l_len = g->l_len, .l_pid = g->l_pid,
                           .l_type = g->l_type == 0 ? F_RDLCK : g->l_type == 1 ? F_WRLCK : F_UNLCK, .l_whence = g->l_whence };
        r = fcntl(fd, cmd == 5 ? F_GETLK : cmd == 6 ? F_SETLK : F_SETLKW, &f);
        if (cmd == 5 && r == 0) { g->l_type = f.l_type == F_RDLCK ? 0 : f.l_type == F_WRLCK ? 1 : 2; g->l_pid = f.l_pid; }
        break;
    }
    default: errno = EINVAL; r = -1; break;
    }
    TL_ERRNO_END();
    return r;
}

static int b_ioctl(int fd, unsigned long req, long arg)
{
    if (req == 0x541B) { int n = 0; int r = ioctl(fd, FIONREAD, &n); if (r == 0) *(int *)arg = n; else TL_ERRNO_END(); return r; }  /* FIONREAD */
    if (req == 0x5421) { int on = *(int *)arg; int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK); return 0; }   /* FIONBIO */
    tl_set_guest_errno(25);                                                                                                                    /* ENOTTY */
    return -1;
}

/* ------------------------------------------------------------ directories */

typedef struct { uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256]; } guest_dirent;
typedef struct { DIR *dir; guest_dirent ent; char **names; uint8_t *types; size_t n, pos; } guest_dir;

/* A folder inside an APK ("jar:file:///.../base.apk!/assets"), listed from the zip: what is directly in it. Unity 6 lists
 * its StreamingAssets this way through System.IO. */
static guest_dir *jar_opendir(const char *p)
{
    jar_entry j;
    if (!jar_lookup(p, &j) || !j.found || !j.dir) return NULL;
    guest_dir *g = calloc(1, sizeof(*g));
    size_t cap = 0, len = strlen(j.entry);
    pthread_mutex_lock(&g_jar_lock);
    for (size_t i = 0; i < g_jar_zip.count; i++) {
        const char *name = g_jar_zip.entries[i].name;
        if (strncmp(name, j.entry, len) || name[len] != '/' || !name[len + 1]) continue;
        const char *rest = name + len + 1, *slash = strchr(rest, '/');
        size_t nl = slash ? (size_t)(slash - rest) : strlen(rest);
        if (!nl) continue;
        bool seen = false;
        for (size_t k = 0; k < g->n && !seen; k++) seen = strlen(g->names[k]) == nl && !strncmp(g->names[k], rest, nl);
        if (seen) continue;
        if (g->n == cap) { cap = cap ? cap * 2 : 32; g->names = realloc(g->names, cap * sizeof(char *)); g->types = realloc(g->types, cap); }
        g->names[g->n] = strndup(rest, nl);
        g->types[g->n] = slash ? DT_DIR : DT_REG;
        g->n++;
    }
    pthread_mutex_unlock(&g_jar_lock);
    return g;
}

static void *b_opendir(const char *p)
{
    if (p && strstr(p, "jar:file:")) { guest_dir *jd = jar_opendir(p); ftrace("opendir", p, jd ? 0 : -1, jd ? 0 : ENOENT); if (!jd) tl_set_guest_errno(2); return jd; }
    char b[1024];
    TL_ERRNO_BEGIN(); DIR *d = opendir(tl_path_resolve(p, b, sizeof(b))); int e = errno; TL_ERRNO_END();
    ftrace("opendir", p, d ? 0 : -1, e);
    if (!d) return NULL;
    guest_dir *g = calloc(1, sizeof(*g));
    g->dir = d;
    return g;
}
static void *b_fdopendir(int fd)
{
    TL_ERRNO_BEGIN(); DIR *d = fdopendir(fd); int e = errno; TL_ERRNO_END();
    ftrace("fdopendir", "", d ? 0 : -1, e);
    if (!d) return NULL;
    guest_dir *g = calloc(1, sizeof(*g));
    g->dir = d;
    return g;
}
static int b_dirfd(void *dp) { guest_dir *g = dp; if (!g->dir) { tl_set_guest_errno(95); return -1; } return dirfd(g->dir); }
static void b_rewinddir(void *dp) { guest_dir *g = dp; if (g->dir) rewinddir(g->dir); else g->pos = 0; }
static void *b_readdir(void *dp)
{
    guest_dir *g = dp;
    if (!g->dir) {
        if (g->pos >= g->n) return NULL;
        g->ent.d_ino = g->pos + 1; g->ent.d_off = (int64_t)g->pos + 1; g->ent.d_reclen = sizeof(guest_dirent); g->ent.d_type = g->types[g->pos];
        snprintf(g->ent.d_name, sizeof(g->ent.d_name), "%s", g->names[g->pos]);
        g->pos++;
        return &g->ent;
    }
    TL_ERRNO_BEGIN(); struct dirent *e = readdir(g->dir); TL_ERRNO_END();
    if (!e) return NULL;
    g->ent.d_ino = e->d_ino; g->ent.d_off = 0; g->ent.d_reclen = sizeof(guest_dirent); g->ent.d_type = e->d_type;
    snprintf(g->ent.d_name, sizeof(g->ent.d_name), "%s", e->d_name);
    return &g->ent;
}
static int b_closedir(void *dp) { guest_dir *g = dp; int r = g->dir ? closedir(g->dir) : 0; for (size_t i = 0; i < g->n; i++) free(g->names[i]); free(g->names); free(g->types); free(g); return r; }

/* scandir: the entries of a directory, optionally filtered and sorted by the guest's own functions (same ISA, called
 * directly). */
static int b_alphasort(const guest_dirent **a, const guest_dirent **b) { return strcoll((*a)->d_name, (*b)->d_name); }
static int b_scandir(const char *path, guest_dirent ***list, int (*filter)(const guest_dirent *), int (*cmp)(const guest_dirent **, const guest_dirent **))
{
    char b[1024];
    TL_ERRNO_BEGIN(); DIR *d = opendir(tl_path_resolve(path, b, sizeof(b))); TL_ERRNO_END();
    if (!d) return -1;
    size_t cap = 16, n = 0;
    guest_dirent **v = malloc(cap * sizeof(*v));
    struct dirent *e;
    while ((e = readdir(d))) {
        guest_dirent *g = calloc(1, sizeof(*g));
        g->d_ino = e->d_ino; g->d_reclen = sizeof(*g); g->d_type = e->d_type;
        snprintf(g->d_name, sizeof(g->d_name), "%s", e->d_name);
        if (filter && !filter(g)) { free(g); continue; }
        if (n == cap) { cap *= 2; v = realloc(v, cap * sizeof(*v)); }
        v[n++] = g;
    }
    closedir(d);
    if (cmp && n > 1) qsort(v, n, sizeof(*v), (int (*)(const void *, const void *))cmp);
    *list = v;
    return (int)n;
}

/* ----------------------------------------------------------------- mmap */

static int prot_filter(int prot, const char *what)
{
    if (prot & PROT_EXEC) {
        /* Executable memory comes from the StikDebug region only; a guest asking for
         * its own gets writable memory and finds out when it jumps there. */
        char note[96]; snprintf(note, sizeof(note), "%s asked for PROT_EXEC memory: refused", what);
        tl_note_once(note);
        prot &= ~PROT_EXEC;
    }
    return prot;
}

/* Anonymous mappings the guest made, so madvise(MADV_DONTNEED) can throw the pages away (the next touch finds zeros).
 * Allocators lean on that; Darwin's MADV_DONTNEED keeps the contents, so the pages are replaced with fresh ones at the
 * same protection instead. */
#define MAX_ANON 4096
static struct { uintptr_t addr; size_t len; } g_anon[MAX_ANON];
static int g_nanon;
static pthread_mutex_t g_anon_lock = PTHREAD_MUTEX_INITIALIZER;

static void anon_add(void *addr, size_t len)
{
    pthread_mutex_lock(&g_anon_lock);
    for (int i = 0; i < g_nanon; i++) {                      /* replace anything this overlaps */
        uintptr_t a = g_anon[i].addr, e = a + g_anon[i].len;
        if ((uintptr_t)addr < e && (uintptr_t)addr + len > a) { g_anon[i] = g_anon[--g_nanon]; i--; }
    }
    if (g_nanon < MAX_ANON) { g_anon[g_nanon].addr = (uintptr_t)addr; g_anon[g_nanon].len = len; g_nanon++; }
    pthread_mutex_unlock(&g_anon_lock);
}

static void anon_zap(uintptr_t addr, size_t len)
{
    uintptr_t end = addr + len;
    pthread_mutex_lock(&g_anon_lock);
    for (int i = 0; i < g_nanon; i++) {
        uintptr_t a = g_anon[i].addr > addr ? g_anon[i].addr : addr;
        uintptr_t e = g_anon[i].addr + g_anon[i].len < end ? g_anon[i].addr + g_anon[i].len : end;
        if (a >= e) continue;
        for (uintptr_t p = a & ~(uintptr_t)16383; p < e;) {           /* walk the regions in the range */
            vm_address_t ra = p; vm_size_t rs = 0;
            vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
            if (vm_region_64(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) break;
            if (ra > p) { p = ra; continue; }
            uintptr_t re = ra + rs < e ? ra + rs : e;
            int prot = (info.protection & VM_PROT_READ ? PROT_READ : 0) | (info.protection & VM_PROT_WRITE ? PROT_WRITE : 0);
            mmap((void *)p, re - p, prot, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
            p = re;
        }
    }
    pthread_mutex_unlock(&g_anon_lock);
}

const char *tl_ld_symbol_at(const void *addr, const char **lib_name, const void **sym_addr);
static int g_mm_trace = -1;
static void mm_trace(const char *what, void *a, size_t l, long x, long y)
{
    if (g_mm_trace < 0) g_mm_trace = getenv("TL_MM_TRACE") ? 1 : 0;
    if (!g_mm_trace) return;
    /* a big one says who asked (the guest's code that called mmap, mprotect or madvise) */
    if (l >= (64u << 20)) {
        void *ra[8]; int n = backtrace(ra, 8);
        const char *who = "?", *lib = NULL; const void *sa = NULL;
        for (int i = 1; i < n; i++) { const char *sym = tl_ld_symbol_at(ra[i], &lib, &sa); if (lib) { who = sym ? sym : "?"; break; } }
        tl_log_line("mm: %s addr=%p len=%#zx %#lx %#lx  <- %s %s", what, a, l, x, y, lib ? lib : "?", who);
        return;
    }
    tl_log_line("mm: %s addr=%p len=%#zx %#lx %#lx", what, a, l, x, y);
}

/*
 * V8's pointer-compression cage is a 4 GiB region aligned to 4 GiB, reserved as twice that with the parts outside the
 * aligned half given back. A sideloaded iPhone app is refused an 8 GiB reservation though it can map 4 GiB. So when the
 * 8 GiB one fails, the caller is given an aligned 4 GiB mapping at the start of an 8 GiB range it believes it owns; the
 * half it gives back was never mapped and is ignored.
 */
#define PHANTOM_HALF ((size_t)4 << 30)
static struct { uintptr_t base; size_t len, real; } g_phantom[4];
static int g_nphantom;

static void *phantom_reserve(size_t len)
{
    if (len != 2 * PHANTOM_HALF || g_nphantom >= 4) return MAP_FAILED;
    void *res = MAP_FAILED;
    /* Reserve 5 GiB, which holds an aligned 4 GiB only when it starts in the first GiB of an alignment period. Mappings
     * come one after another, so a miss is kept (it moves the next on by a GiB) and given back once there is a hit. */
    void *held[8]; int nheld = 0;
    const size_t S = PHANTOM_HALF + ((size_t)1 << 30);
    for (int t = 0; t < 8 && res == MAP_FAILED; t++) {
        void *p = mmap(NULL, S, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) break;
        uintptr_t b = (uintptr_t)p, start = (b + PHANTOM_HALF - 1) & ~(PHANTOM_HALF - 1), end = start + PHANTOM_HALF;
        if (end <= b + S) {
            if (start > b) munmap(p, start - b);
            if (b + S > end) munmap((void *)end, b + S - end);
            res = (void *)start;
        } else held[nheld++] = p;
    }
    for (int i = 0; i < nheld; i++) munmap(held[i], S);
    if (res == MAP_FAILED) {                                  /* a 5 GiB mapping is refused too: try 4 GiB where it happens to fall aligned */
        void *p = mmap(NULL, PHANTOM_HALF, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p != MAP_FAILED && ((uintptr_t)p & (PHANTOM_HALF - 1))) { munmap(p, PHANTOM_HALF); p = MAP_FAILED; }
        for (uintptr_t hint = (uintptr_t)3 << 32; p == MAP_FAILED && hint < ((uintptr_t)1 << 36); hint += PHANTOM_HALF) {
            void *q = mmap((void *)hint, PHANTOM_HALF, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (q == (void *)hint) p = q;
            else if (q != MAP_FAILED) munmap(q, PHANTOM_HALF);
        }
        res = p;
    }
    if (res == MAP_FAILED) return MAP_FAILED;
    g_phantom[g_nphantom].base = (uintptr_t)res; g_phantom[g_nphantom].len = len; g_phantom[g_nphantom].real = PHANTOM_HALF;
    g_nphantom++;
    tl_log_line("mm: an 8 GiB reservation was refused; gave an aligned 4 GiB one at %p instead", res);
    return res;
}

/* How much of [a, a+l) is really mapped: the part of a phantom range past its real half is not. */
static size_t phantom_clamp(uintptr_t a, size_t l)
{
    for (int i = 0; i < g_nphantom; i++) {
        uintptr_t b = g_phantom[i].base, real_end = b + g_phantom[i].real, end = b + g_phantom[i].len;
        if (a >= b && a < end) return a >= real_end ? 0 : (a + l > real_end ? real_end - a : l);
    }
    return l;
}

/* Whether only the JIT region can execute here: the phone (iOS), or a Mac told to behave like one (TL_PHONE_JIT=1). */
static bool phone_jit(void)
{
#if defined(__APPLE__) && TARGET_OS_IPHONE
    return true;
#else
    static int on = -1;
    if (on < 0) on = getenv("TL_PHONE_JIT") ? 1 : 0;
    return on;
#endif
}

/* The guest library symbol that asked for something, from this thread's return addresses. The layer's own
 * frames (the syscall shim) come first and name no guest library, so the first frame that does is the
 * guest's call site. Best effort: "?" when nothing names it. */
static const char *guest_caller_site(const char **lib_out)
{
    void *ra[16];
    int n = backtrace(ra, 16);
    for (int i = 1; i < n; i++) {
        const char *lib = NULL; const void *sa = NULL;
        const char *sym = tl_ld_symbol_at(ra[i], &lib, &sa);
        if (lib) { if (lib_out) *lib_out = lib; return sym ? sym : "?"; }
    }
    if (lib_out) *lib_out = "?";
    return "?";
}

/* Read `<data dir>/.husk-jit-writable` for the app now running, once, lazily. When it names THIS app
 * (package and version match), its writable mappings are handed the executable view again -- its old
 * behaviour -- so a JIT title still runs. A note for a different app or version is ignored, never
 * guessed at. Identity is not known until tl_hle_configure has run, so while it is empty the read is
 * left pending and retried on the next call. */
static void rw_exec_memory_load(void)
{
    if (g_rw_mem_loaded) return;
    const char *pkg = tl_hle_package();
    if (!pkg || !pkg[0]) return;                 /* identity not known yet: retry later */
    g_rw_mem_loaded = 1;
    g_rw_app_needs_exec = false;
    int ver = tl_hle_version_code();
    char path[1120];
    snprintf(path, sizeof(path), "%s/.husk-jit-writable", tl_data_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    char hdr[64] = "", pkg_f[256] = "", line[320];
    int ver_f = -1;
    if (fgets(line, sizeof(line), f)) sscanf(line, "%63s", hdr);
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "pkg ", 4)) sscanf(line + 4, "%255s", pkg_f);
        else if (!strncmp(line, "version ", 8)) ver_f = atoi(line + 8);
    }
    fclose(f);
    if (strcmp(hdr, "husk-jit-writable") || strcmp(pkg_f, pkg) || ver_f != ver) {
        if (hdr[0])
            tl_log_line("bionic: a remembered RW-exec note at %s is for %s version %d, not %s version %d: ignored (the app gets native stores)",
                        path, pkg_f[0] ? pkg_f : "(unknown)", ver_f, pkg, ver);
        return;
    }
    g_rw_app_needs_exec = true;
    tl_log_line("bionic: %s version %d is remembered as an app that writes code into writable memory and marks it executable: its writable mappings are handed the EXECUTABLE view again -- its old behaviour (stores there trap through the store guard, but it runs). Clear this by deleting %s",
                pkg, ver, path);
}

/* Remember, in the app's own container, that this app wanted execute on writable memory, so its next
 * launch gets the executable view for its writable mappings. Written to a sibling `.tmp` then renamed
 * into place, so a reader never sees a half-written file. Also takes effect for the REST of this run:
 * once the app has shown it is a JIT, further writable mappings are handed `rx` (its old behaviour). */
static void rw_exec_memory_record(uintptr_t addr, size_t len)
{
    g_rw_app_needs_exec = true;
    const char *pkg = tl_hle_package();
    char path[1120], tmp[1160];
    snprintf(path, sizeof(path), "%s/.husk-jit-writable", tl_data_dir());
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        tl_log_line("bionic: could not remember at %s that %s writes code into writable memory: %s (it will be handed the writable view again next launch)",
                    path, pkg && pkg[0] ? pkg : "(unknown)", strerror(errno));
        return;
    }
    fprintf(f, "husk-jit-writable 1\n");
    fprintf(f, "pkg %s\n", pkg && pkg[0] ? pkg : "(unknown)");
    fprintf(f, "version %d\n", tl_hle_version_code());
    fprintf(f, "when %lld\n", (long long)time(NULL));
    fprintf(f, "address %#llx\n", (unsigned long long)addr);
    fprintf(f, "length %#zx\n", len);
    fflush(f);
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        tl_log_line("bionic: the RW-exec note %s could not be put in place: %s", path, strerror(errno));
        remove(tmp);
        return;
    }
    tl_log_line("bionic: remembered in %s that %s version %d writes code into writable memory and marks it executable: its writable mappings get the EXECUTABLE view on the next launch (delete that file to forget it)",
                path, pkg && pkg[0] ? pkg : "(unknown)", tl_hle_version_code());
}

/* THE HONEST FAILURE FOR THE JIT CASE. A guest asked for execute on a page in a mapping it was handed
 * WRITABLE (b_mmap's RW-only branch below), and execute cannot be granted to that view: the writable
 * view is a vm_remap alias and the debugger's per-entry execute grant is on the other view, so
 * tl_xmem_allow_exec refuses it. The refusal itself is unchanged (EACCES, never a pretend success);
 * this line makes it LOUD, once, naming the app, the address and the caller, and records the app so it
 * gets the executable view back on its next launch. */
static void rw_exec_refuse_log(void *a, size_t l)
{
    static int said;
    if (said++) return;
    const char *lib = "?";
    const char *sym = guest_caller_site(&lib);
    const char *pkg = tl_hle_package();
    tl_log_line("husk: RW-exec refused for %s (version %d) at %p (%#zx bytes), asked by %s %s: this app writes code into writable memory and then marks it executable: it needs the executable view and will not run on this device while writable mappings are handed the writable view. The app is remembered and gets the executable view on its next launch",
                pkg && pkg[0] ? pkg : "(unknown)", tl_hle_version_code(), a, l, lib, sym);
    rw_exec_memory_record((uintptr_t)a, l);
}

/* File mappings at an offset Android allows (4 KiB pages) but this host does not (16 KiB): mapped from the host page below the
 * offset, the guest given the address its offset lands on. munmap gets back what was really mapped. */
static struct { uintptr_t guest, host; size_t len; } g_shifted[64];
static int g_nshifted;
static pthread_mutex_t g_shifted_lock = PTHREAD_MUTEX_INITIALIZER;
static void *shifted_file_map(void *addr, size_t len, int prot, int df, int fd, long off)
{
    long pg = getpagesize(), delta = off & (pg - 1);
    if (df & MAP_FIXED) {
        /* a fixed private mapping can not move: anonymous memory with the file's bytes read into it */
        if ((df & 0x3) != MAP_PRIVATE) { errno = EINVAL; return MAP_FAILED; }
        void *r = mmap(addr, len, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
        if (r == MAP_FAILED) return r;
        size_t got = 0;
        while (got < len) { ssize_t n = pread(fd, (char *)r + got, len - got, off + (off_t)got); if (n <= 0) break; got += (size_t)n; }
        if (prot != (PROT_READ | PROT_WRITE)) mprotect(r, len, prot);
        return r;
    }
    char *h = mmap(NULL, len + (size_t)delta, prot, df, fd, off - delta);
    if (h == MAP_FAILED) return MAP_FAILED;
    pthread_mutex_lock(&g_shifted_lock);
    if (g_nshifted < 64) { g_shifted[g_nshifted].guest = (uintptr_t)h + (uintptr_t)delta; g_shifted[g_nshifted].host = (uintptr_t)h; g_shifted[g_nshifted].len = len + (size_t)delta; g_nshifted++; }
    pthread_mutex_unlock(&g_shifted_lock);
    tl_note_once("a file was mapped at an offset that is not a multiple of this host's page size: mapped from the page below");
    return h + delta;
}
static bool shifted_unmap(void *a, int *ret)
{
    pthread_mutex_lock(&g_shifted_lock);
    for (int i = 0; i < g_nshifted; i++) if (g_shifted[i].guest == (uintptr_t)a) {
        uintptr_t h = g_shifted[i].host; size_t l = g_shifted[i].len;
        g_shifted[i] = g_shifted[--g_nshifted];
        pthread_mutex_unlock(&g_shifted_lock);
        *ret = munmap((void *)h, l);
        return true;
    }
    pthread_mutex_unlock(&g_shifted_lock);
    return false;
}
static void *b_mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    tl_hp_bump(TL_HP_MMAP);
    rw_exec_memory_load();                          /* the app's remembered RW-exec behaviour, read once */
    /* A hooking library's trampolines (Geode): executable memory is a piece of the JIT region, written through the other view. */
    if ((prot & PROT_EXEC) && (flags & 0x20) && !(flags & 0x10) && tl_codewrite_enabled()) {
        uint8_t *rx, *rw;
        size_t n = (len + TL_XMEM_PAGE - 1) & ~(size_t)(TL_XMEM_PAGE - 1);
        if (tl_xmem_alloc(n, &rx, &rw)) {
            memset(rw, 0, n);
            tl_log_line("mm: %#zx bytes of executable memory for the guest at %p", n, (void *)rx);
            return rx;
        }
    }
    /* A JIT's code memory where only the JIT region can execute (iOS 26+ lets nothing else run): LuaJIT maps it near its
     * own code with a hint, writes machine code, then mprotects it executable -- which on a phone "succeeds" and then
     * faults. An anonymous, code-sized, writable mapping hinted near the region comes from the region instead. */
    if (addr && !(prot & PROT_EXEC) && (prot & PROT_WRITE) && (flags & 0x20) && !(flags & 0x10) && len <= (32u << 20)
        && tl_codewrite_enabled() && phone_jit() && tl_xmem_near(addr, 256u << 20)) {
        uint8_t *rx, *rw;
        size_t n = (len + TL_XMEM_PAGE - 1) & ~(size_t)(TL_XMEM_PAGE - 1);
        if (tl_xmem_alloc(n, &rx, &rw)) {
            memset(rw, 0, n);
            tl_note_once("a JIT's code memory (mmap hinted near the code) comes from the JIT region");
            mm_trace("mmap-jit", rx, len, prot, flags);
            /* The guest's OWN writable memory. Handed the EXECUTABLE view (rx), every store into it
             * TRAPS: the store guard decodes the instruction and carries it through the writable twin at
             * tl_xmem_delta() -- a signal, a decode and a twin store per 16 bytes of DATA. That traffic,
             * ~147,000 trapped stores/second, is what capped robot gf at 40 FPS and made its 49-second
             * load to the main menu (data-out-of-the-executable-view.md §2: the traps are the guest's OWN
             * spans, not library data pages and not the loader's placement). The pages are the same pages;
             * the ONLY change is which of the region's two views the guest's pointer NAMES. A store into
             * the writable view is a native store and does not fault.
             *
             * The cost, stated plainly: this branch is here FOR a JIT (the comment above). A guest handed
             * `rw` that later writes code and calls mprotect(PROT_EXEC) on it reaches tl_xmem_allow_exec,
             * which cannot grant execute to this vm_remap alias and refuses with EACCES (b_mprotect). That
             * refusal is honest and LOUD (rw_exec_refuse_log), and the app is REMEMBERED
             * (rw_exec_memory_record) so on its next launch it is handed the executable view again -- its
             * old behaviour -- and a JIT title still runs. Only an app that ignores errno would call into
             * non-executable memory; that is why the refusal is never silent. */
            if (g_rw_app_needs_exec) return rx;      /* a remembered JIT app: its old behaviour */
            return rw;                                /* the writable view: stores are native, no trap */
        }
    }
    int df = flags & 0x3;                                   /* MAP_SHARED / MAP_PRIVATE */
    if (flags & 0x10)   df |= MAP_FIXED;
    if (flags & 0x20)   df |= MAP_ANON;
    /* A redirected descriptor mapped directly: mmap would take the APK's own bytes at the guest's own offset, not the
     * entry's. It is left exactly as it is (Mono's FileStream reads, never mmaps) but said once, so an entry read through a
     * mapping is never silent. */
    if (!(flags & 0x20) && vfd_is(fd)) {
        static bool said[4096];
        if (fd >= 0 && fd < 4096 && !said[fd]) {
            said[fd] = true;
            tl_log_line("husk: virtual file: mmap on redirected fd %d maps the APK itself (%#zx bytes at offset %ld), not the entry %s: a read through that mapping is NOT served by the redirect",
                        fd, len, off, g_vfd[fd].name[0] ? g_vfd[fd].name : "(unnamed)");
        }
    }
    TL_ERRNO_BEGIN();
    static int force_phantom = -1;
    if (force_phantom < 0) force_phantom = getenv("TL_MM_PHANTOM") != NULL;          /* a Mac pretending to be a phone */
    static size_t max_map = (size_t)-2;                                              /* TL_MM_MAX_GIB: refuse bigger single mappings, as a phone does */
    if (max_map == (size_t)-2) max_map = getenv("TL_MM_MAX_GIB") ? (size_t)atoi(getenv("TL_MM_MAX_GIB")) << 30 : (size_t)-1;
    bool refuse = len > max_map && (flags & 0x20) && !(flags & 0x10);
    if (refuse) { tl_log_line("mm: refusing %#zx bytes (TL_MM_MAX_GIB)", len); errno = ENOMEM; }
    bool shifted = !(flags & 0x20) && fd >= 0 && (off & (getpagesize() - 1)) != 0 && (off & 4095) == 0;
    void *r = refuse || (force_phantom && len == 2 * PHANTOM_HALF && prot == 0 && (flags & 0x20) && !(flags & 0x10)) ? MAP_FAILED
            : shifted ? shifted_file_map(addr, len, prot_filter(prot, "mmap"), df, fd, off)
            : mmap(addr, len, prot_filter(prot, "mmap"), df, (flags & 0x20) ? -1 : fd, off);
    if (r == MAP_FAILED && (errno == ENOMEM || force_phantom) && prot == 0 && (flags & 0x20) && !(flags & 0x10)) { r = phantom_reserve(len); if (r != MAP_FAILED) errno = 0; }
    TL_ERRNO_END();
    /* a file mapped executable (libffi trampolines): a phone cannot make file pages executable and a library's file offsets
     * are not where the loader put its code, so the mapping fails here as it would there, and libffi takes anonymous
     * executable memory instead (a piece of the JIT region) */
    if (r != MAP_FAILED && (prot & PROT_EXEC) && !(flags & 0x20)) {
        tl_note_once("a file was mapped executable (libffi trampolines): refused, so anonymous executable memory is used");
        TL_ERRNO_BEGIN(); munmap(r, len); TL_ERRNO_END();
        tl_set_guest_errno(13); r = MAP_FAILED;
    }
    if (r != MAP_FAILED && (flags & 0x20)) anon_add(r, len);
    mm_trace("mmap", r, len, prot, flags);
    if (r == MAP_FAILED) tl_log_line("mm: mmap FAILED len=%#zx prot=%d flags=%#x errno=%d", len, prot, flags, errno);
    return r;
}
static int b_munmap(void *a, size_t l)
{
    if (tl_xmem_contains(a)) return 0;                       /* JIT memory handed out above: kept, as the region only grows */
    { int sr; if (shifted_unmap(a, &sr)) { mm_trace("munmap", a, l, sr, errno); return sr; } }
    size_t real = phantom_clamp((uintptr_t)a, l);
    if (real == 0) return 0;                                 /* the half of a phantom range that was never mapped */
    TL_ERRNO_BEGIN(); int r = munmap(a, real); TL_ERRNO_END(); mm_trace("munmap", a, real, r, errno); return r;
}
/* Whether a range is inside memory the guest mapped for itself (not a library's own pages). */
static bool anon_contains(uintptr_t addr, size_t len)
{
    bool in = false;
    pthread_mutex_lock(&g_anon_lock);
    for (int i = 0; i < g_nanon && !in; i++) in = addr >= g_anon[i].addr && addr + len <= g_anon[i].addr + g_anon[i].len;
    pthread_mutex_unlock(&g_anon_lock);
    return in;
}

/* A guest turning its own anonymous memory executable is a JIT (LuaJIT, a regex engine) and cannot be given that; saying so
 * lets it fall back to its interpreter, where pretending to succeed would have it jump into data. */
const char *tl_ld_symbol_at(const void *addr, const char **lib_name, const void **sym_addr);
static int b_mprotect(void *a, size_t l, int prot)
{
    rw_exec_memory_load();                          /* the app's remembered RW-exec behaviour, read once */
    /* Code a hooking library is about to patch: its permissions stay as they are, and its stores are carried out through
     * the writable view as they fault (husk-tl-codewrite.c). */
    if (tl_codewrite_enabled() && tl_xmem_contains(a)) {
        /* A page the guest names with PROT_EXEC that is in the WRITABLE view is its own JIT arena (the loader retargets
         * the guest's adrp at the writable view, husk-tl-ld.c). The executable view of those pages already runs, but the
         * address the guest holds does not, so execute is added to the writable view -- and only claimed if that really
         * happened: tl_xmem_allow_exec fetch-tests the page (the verdict is the fetch, not vm_protect's return code and
         * not the read-back printed beside it). */
        if ((prot & PROT_EXEC) && !tl_xmem_is_rx(a)) {
            bool ok = tl_xmem_allow_exec(a, l);
            static int said;
            if (said++ < 1)
                tl_log_line("husk: xmem exec on the writable view a=%p l=%#zx prot=%d (executable view %p): %s",
                            a, l, prot, (void *)((uintptr_t)a - (uintptr_t)tl_xmem_delta()),
                            ok ? "made executable (the guest's JIT arena)" : "REFUSED; the guest's arena stays non-executable");
            if (!ok) { rw_exec_refuse_log(a, l); tl_set_guest_errno(13); return -1; }  /* EACCES, named: an honest failure, not a lie */
        }
        mm_trace("mprotect", a, l, prot, 0);
        return 0;
    }
    if (prot & PROT_EXEC) {
        /* the host's own code (a guest that patches "the linker", which here is Apple's): never touched */
        Dl_info di;
        if (dladdr(a, &di) && di.dli_fname && !tl_xmem_contains(a)) {
            { const char *lib = NULL; const void *sa = NULL; const char *sym = tl_ld_symbol_at(__builtin_return_address(0), &lib, &sa);
              static int said; if (said++ < 4) tl_log_line("bionic: mprotect(%p, %#zx, %d) on the host's code (%s %s) asked by %s %s: refused", a, l, prot,
                  di.dli_fname ? strrchr(di.dli_fname, '/') + 1 : "?", di.dli_sname ? di.dli_sname : "?", lib ? lib : "?", sym ? sym : "?"); }
            tl_set_guest_errno(13);
            return -1;
        }
        /* the guest's own memory it wrote code into (Meta's and ByteDance's stubs, a JIT): made executable when the system
         * lets a process do that, write access dropped if both were asked for (W^X); refused otherwise so a JIT falls back
         * to its interpreter. TL_NO_GUEST_JIT=1 always refuses. */
        /* the calling thread's own stack (liblinkernsbypass asks for RWX on the page it keeps a local in): left writable and
         * as it is -- dropping write there for W^X would leave the thread unable to push a frame. Heap pages still get the
         * real attempt below. */
        {
            pthread_t me = pthread_self();
            uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(me), low = top - pthread_get_stacksize_np(me);
            if ((uintptr_t)a >= low && (uintptr_t)a < top) {
                tl_note_once("mprotect asked to make a stack page executable: left as it is");
                mm_trace("mprotect", a, l, prot, 0);
                return 0;
            }
        }
        static int allow = -1;
        if (allow < 0) allow = getenv("TL_NO_GUEST_JIT") ? 0 : 1;
        int r = -1;
        if (allow) {
            TL_ERRNO_BEGIN();
            r = mprotect(a, l, prot & (PROT_READ | PROT_WRITE | PROT_EXEC));
            if (r != 0 && (prot & PROT_WRITE)) r = mprotect(a, l, (prot & ~PROT_WRITE) & (PROT_READ | PROT_EXEC));
            TL_ERRNO_END();
            if (r == 0) { sys_icache_invalidate(a, l); tl_note_once("the guest made memory of its own executable (generated code)"); }
        }
        if (r != 0) {
            tl_note_once("mprotect asked to make the guest's own memory executable: refused");
            tl_set_guest_errno(13);                                                                                        /* EACCES */
            return -1;
        }
        mm_trace("mprotect", a, l, prot, 0);
        return 0;
    }
    TL_ERRNO_BEGIN(); int r = mprotect(a, l, prot_filter(prot, "mprotect")); int e = errno; TL_ERRNO_END(); mm_trace("mprotect", a, l, prot, r ? e : 0); return r; }
static int b_madvise(void *a, size_t l, int adv)
{
    mm_trace("madvise", a, l, adv, 0);
    if (adv == 4) { anon_zap((uintptr_t)a, l); return 0; }  /* MADV_DONTNEED: zero-fill on next touch */
    return 0;                                                /* the rest are hints */
}
static void *b_mremap(void *old, size_t olds, size_t news, int flags, void *newaddr)
{
    mm_trace("mremap", old, olds, (long)news, flags);
    (void)flags; (void)newaddr;
    void *n = mmap(NULL, news, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (n == MAP_FAILED) { tl_set_guest_errno(12); return (void *)-1; }
    anon_add(n, news);
    memcpy(n, old, olds < news ? olds : news);
    munmap(old, olds);
    return n;
}

/* ------------------------------------------------------------------- time */

static clockid_t clock_to_darwin(int id)
{
    switch (id) {
    case 0: return CLOCK_REALTIME;
    case 1: case 4: case 6: case 7: return CLOCK_MONOTONIC;
    case 2: return CLOCK_PROCESS_CPUTIME_ID;
    case 3: return CLOCK_THREAD_CPUTIME_ID;
    case 5: return CLOCK_REALTIME;
    default: return CLOCK_MONOTONIC;
    }
}
static int b_clock_gettime(int id, struct timespec *ts) { tl_hp_bump(TL_HP_CLOCK_GETTIME); TL_ERRNO_BEGIN(); int r = clock_gettime(clock_to_darwin(id), ts); TL_ERRNO_END(); return r; }
static int b_clock_getres(int id, struct timespec *ts) { TL_ERRNO_BEGIN(); int r = clock_getres(clock_to_darwin(id), ts); TL_ERRNO_END(); return r; }
static int b_gettimeofday(int64_t *tv, void *tz)
{
    tl_hp_bump(TL_HP_GETTIMEOFDAY);
    struct timeval t; (void)tz;
    gettimeofday(&t, NULL);
    if (tv) { tv[0] = t.tv_sec; tv[1] = t.tv_usec; }
    return 0;
}
static int b_nanosleep(const struct timespec *req, struct timespec *rem) { TL_ERRNO_BEGIN(); int r = nanosleep(req, rem); TL_ERRNO_END(); return r; }
static int b_usleep(unsigned us) { return usleep(us); }
/* clock_nanosleep: relative, or until a time on the clock (TIMER_ABSTIME); returns the error, as POSIX says. Linux's REALTIME 0,
 * MONOTONIC 1, BOOTTIME 7 (the others are taken as monotonic). */
static int b_clock_nanosleep(int clock, int flags, const struct timespec *req, struct timespec *rem)
{
    struct timespec d = *req;
    if (flags & 1) {
        struct timespec now; clock_gettime(clock == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
        int64_t ns = ((int64_t)req->tv_sec - now.tv_sec) * 1000000000ll + (req->tv_nsec - now.tv_nsec);
        if (ns <= 0) return 0;
        d.tv_sec = ns / 1000000000ll; d.tv_nsec = ns % 1000000000ll;
        rem = NULL;
    }
    if (nanosleep(&d, rem)) return tl_errno_to_guest(errno);
    return 0;
}

/* ---------------------------------------------------------------- signals */

/* bionic's LP64 struct sigaction: flags, handler, 64-bit mask, restorer. */
typedef struct { int sa_flags; int pad; void *handler; uint64_t sa_mask; void *restorer; } guest_sigaction;
typedef void (*guest_handler)(int, void *, void *);

static struct { void *handler; int flags; } g_guest_sig[32];

/* The address of THIS image's guest-handler table, for the self-test's image-identity proof: if the layer is loaded
 * twice, each copy's table is at a different address, and a handler installed by one is invisible to the other. */
void *tl_signal_guest_table_image(void) { return (void *)&g_guest_sig[0]; }

static int g_sig_trace = -1;
static void sig_trace(const char *what, int g)
{
    if (g_sig_trace < 0) g_sig_trace = getenv("TL_SIGNAL_TRACE") ? 1 : 0;
    if (!g_sig_trace) return;
    char nm[32] = ""; pthread_getname_np(pthread_self(), nm, sizeof(nm));
    tl_log_line("signal[%s]: %s %d", nm, what, g);
}

/*
 * ------------------------------------------------- the mask a guest thread runs guest code with
 *
 * On bionic a new thread inherits the creating thread's signal mask, and Darwin's pthread_create does the same (XNU
 * copies the creating thread's uth_sigmask into the new uthread). So when a GUEST thread creates a guest thread, the
 * inheritance the guest expects comes for free. It does NOT when the creator is one of the LAYER's own threads: the
 * mask such a thread carries came from the host process -- UIKit, SpringBoard, the app's own bootstrap -- never from
 * the guest, and bionic would never have had it. Nothing in the layer ever calls pthread_sigmask/sigprocmask on a
 * guest thread's behalf, so a blocked bit on a guest thread can only be one that rode in this way, and it is exactly
 * the last reading this bug has left:
 *
 *   il2cpp's collector (Boehm) stops the world by signalling EVERY registered thread and waits for each one's handler
 *   to run. A thread whose mask has Darwin 29 -- bionic 30 / SIGPWR, with 24 / SIGXCPU beside it -- BLOCKED leaves its
 *   thread-directed signal PENDING: Darwin runs no handler and nothing unblocks it, so the collector never gets its ack,
 *   the engine's main thread never returns from GC_stop_world, and the app never asks for another frame.
 *
 * tl_guest_mask_default() puts the CALLING thread's mask where a bionic thread starts: empty. It is called the moment
 * layer code hands off to guest code -- at every guest thread's entry (start_thunk) and at each of the layer's own
 * threads that runs the guest (the Java main thread, a Java thread, the DVM boot thread, the native start thread, the
 * GL thread) -- so a guest thread runs guest code with the mask the guest expects and the host's mask cannot ride in.
 * It is ONE pthread_sigmask on a thread's ENTRY: never per call, never per frame, and it takes no lock unless a line is
 * written.
 *
 * It also puts the fact in the ledger, ONCE per signal: a thread that arrives with a signal BLOCKED that the guest has
 * armed a handler for is the mask this bug is made of, and the line names it. That is the ONLY place the mask is
 * visible at all -- a blocked thread-directed signal is invisible on the send side, which is why the ledger's send
 * counts read as "armed, accepted, never ran".
 */
static atomic_int g_mask_reported;   /* threads that have printed their entry mask: a fixed cap, so a thread storm cannot flood the log */

void tl_guest_mask_default(void)
{
    sigset_t cur;
    if (pthread_sigmask(SIG_SETMASK, NULL, &cur) != 0) return;
    uint32_t m; memcpy(&m, &cur, sizeof(m));

    /* MEASURE the mask, do not infer it: this thread reads its OWN mask (no Darwin call reads another thread's) and
     * reports what it was BORN with, UNCONDITIONALLY and once per thread (capped). The earlier form only named a bit when
     * the guest had ALREADY armed that signal -- never true at a thread's entry -- so it never fired and the inherited
     * mask stayed invisible; the send/run ledger could then only guess ("SENDS > RUNS ... at least one thread has the
     * Darwin signal BLOCKED"). This line turns that guess into a reading: 0x0 is the guest's own empty mask, anything
     * else is HOST code's mask that rode in by inheritance. */
    tl_signal_note_mask_entry(m);
    if (atomic_fetch_add(&g_mask_reported, 1) < 16) {
        char me[32]; pthread_getname_np(pthread_self(), me, sizeof(me));
        if (!me[0]) snprintf(me, sizeof(me), "?");
        char bits[220] = ""; size_t bl = 0;
        for (int d = 1; d < 32 && bl + 16 < sizeof(bits); d++) {
            if (!(m & (1u << (d - 1)))) continue;
            int g = tl_signal_from_darwin(d);
            bl += (size_t)snprintf(bits + bl, sizeof(bits) - bl, "%sDarwin %d%s%s",
                                   bl ? ", " : "", d, g ? "=bionic " : "", "");
            if (g) bl += (size_t)snprintf(bits + bl, sizeof(bits) - bl, "%d", g);
        }
        tl_log_line("signal-mask: thread '%s' is about to run guest code; it READ ITS OWN mask here: 0x%08x (%s) -- %s",
                    me, m, m ? bits : "empty",
                    m ? "HOST bits rode in by inheritance (no guest blocked them, and no layer code blocks them); clearing them now"
                      : "the guest's own empty mask");
    }

    for (int g = 1; g < 32; g++) {
        if (!g_guest_sig[g].handler || g_guest_sig[g].handler == (void *)1) continue;
        int d = tl_signal_to_darwin(g);
        if (d > 0 && (m & (1u << (d - 1)))) tl_signal_note_arrived_blocked(g, d);
    }
    if (m) {
        sigset_t empty; sigemptyset(&empty);
        int cr = pthread_sigmask(SIG_SETMASK, &empty, NULL);
        /* VERIFY the clear by reading the mask back -- a clear that did not take must not be silently indistinguishable
         * from a clean thread: a signal blocked on a thread that runs guest code leaves every delivery of it PENDING and
         * its handler never runs, which is exactly a stop-the-world handshake waiting forever. */
        sigset_t back; uint32_t mb = 0;
        if (pthread_sigmask(SIG_SETMASK, NULL, &back) == 0) memcpy(&mb, &back, sizeof(mb));
        else mb = 0xffffffffu;
        if (cr != 0 || mb) tl_signal_note_mask_clear_failed(cr, (unsigned long)mb);
    }
}

/* The context a Linux arm64 handler is given (bionic's ucontext_t, the kernel's layout): built from Darwin's for each delivery and
 * copied back after, so a handler that reads the interrupted registers (Mono's thread suspension samples the pc and sp, and
 * gives up and retries forever when they are garbage) or changes them sees what a phone would show it. */
typedef struct { uint64_t fault_address, regs[31], sp, pc, pstate; uint8_t reserved[4096] __attribute__((aligned(16))); } guest_sigcontext;
typedef struct {
    uint64_t uc_flags; void *uc_link;
    struct { void *ss_sp; int ss_flags; int pad; size_t ss_size; } uc_stack;
    uint64_t uc_sigmask; uint8_t sigmask_pad[120];
    guest_sigcontext uc_mcontext __attribute__((aligned(16)));
} guest_ucontext;
_Static_assert(__builtin_offsetof(guest_ucontext, uc_mcontext) == 176, "bionic's ucontext_t puts uc_mcontext at 176");
_Static_assert(__builtin_offsetof(guest_sigcontext, reserved) == 288, "the kernel's sigcontext puts __reserved at 288");

/*
 * The per-thread buffer a delivery builds bionic's ucontext_t in. It is PREALLOCATED, part of the thread's own fixed
 * storage, allocated when the thread is created and NEVER by the handler: the old handler did
 * `if (!g) g = calloc(1, sizeof(*g))` -- a 4.5 KiB allocation INSIDE a signal handler. calloc is not async-signal-safe
 * (it takes the allocator's lock and walks free lists), so if the interrupted thread already held that lock -- and a
 * stop-the-world path is exactly where that happens -- the handler deadlocks and the thread the signal was meant to
 * park never answers. A fixed record per thread removes the allocation entirely; the nesting depth gives a signal that
 * arrives inside the handler a buffer of its own, still with no allocation.
 */
#define GUEST_UC_DEPTH 2
static __thread guest_ucontext t_guest_uc[GUEST_UC_DEPTH];
static __thread int t_guest_uc_depth;

/*
 * ---------------------------------------------------------------- the delivery handler
 *
 * Runs on the guest's own thread in signal context and must be async-signal-safe END TO END. Every call it makes is
 * audited here; each one that would NOT have been safe is named as fixed or removed:
 *
 *   tl_signal_from_darwin      a table walk and one plain int read (g_pwr_darwin)              safe
 *   tl_signal_note_enter/…     ONE relaxed atomic add each; no lock, no name lookup, no log     safe
 *   memset                     the compiler's own; no lock                                       safe
 *   arm_thread_state64_get/set inline reads/writes of _STRUCT_ARM_THREAD_STATE64                safe
 *   the guest handler          the guest's own code, in signal context by definition            the guest's contract
 *
 * REMOVED because they are NOT async-signal-safe -- each could deadlock the very stop-the-world path this handler
 * serves:
 *   calloc / free              the allocator's lock                                        FIXED: a fixed per-thread record
 *   sig_trace()                pthread_getname_np + tl_log_line (vsnprintf, stdio lock)    FIXED: no logging here
 *   tl_signal_note_run()       pthread_mutex_lock + tl_log_line on the first two runs      FIXED: lock-free counters
 * The first events are no longer logged FROM the handler; the ledger records them as counters and tl_signal_report
 * prints them (with the thread name) from a normal thread, where logging is safe. See step24-the-discriminator.md.
 */
static void host_signal_entry(int dsig, siginfo_t *info, void *uctx)
{
    int gsig = tl_signal_from_darwin(dsig);
    tl_signal_note_enter(gsig);              /* stage 1: the host dispatcher was ENTERED (gsig 0 = not a guest signal) */
    if (gsig < 1 || gsig > 31) return;       /* a Darwin number no bionic signal claims: not ours to run */
    void *h = g_guest_sig[gsig].handler;
    if (!h || h == (void *)1) return;        /* SIG_DFL / SIG_IGN: nothing recorded to run */
    tl_signal_note_ptr_found(gsig);          /* stage 2: a guest handler pointer is recorded for it */
    ucontext_t *uc = uctx;
    if (!uc || !uc->uc_mcontext) {           /* nothing to convert: hand the guest what Darwin gave us */
        tl_signal_note_called(gsig);
        ((guest_handler)h)(gsig, info, uctx);
        tl_signal_note_returned(gsig);
        return;
    }
    int depth = t_guest_uc_depth;
    if (depth < 0 || depth >= GUEST_UC_DEPTH) {   /* nested past the pool: NEVER allocate; convert nothing */
        tl_signal_note_called(gsig);
        ((guest_handler)h)(gsig, info, uctx);
        tl_signal_note_returned(gsig);
        return;
    }
    guest_ucontext *g = &t_guest_uc[depth];
    t_guest_uc_depth = depth + 1;
    _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    memset(g, 0, offsetof(guest_ucontext, uc_mcontext) + offsetof(guest_sigcontext, reserved));
    g->uc_stack.ss_sp = uc->uc_stack.ss_sp; g->uc_stack.ss_size = uc->uc_stack.ss_size; g->uc_stack.ss_flags = uc->uc_stack.ss_flags;
    for (int r = 0; r < 29; r++) g->uc_mcontext.regs[r] = ss->__x[r];
    g->uc_mcontext.regs[29] = arm_thread_state64_get_fp(*ss);
    g->uc_mcontext.regs[30] = arm_thread_state64_get_lr(*ss);
    g->uc_mcontext.sp = arm_thread_state64_get_sp(*ss);
    g->uc_mcontext.pc = arm_thread_state64_get_pc(*ss);
    g->uc_mcontext.pstate = ss->__cpsr;
    g->uc_mcontext.fault_address = info ? (uint64_t)(uintptr_t)info->si_addr : 0;
    tl_signal_note_called(gsig);             /* stage 3: the guest handler is being CALLED */
    ((guest_handler)h)(gsig, info, g);
    tl_signal_note_returned(gsig);           /* stage 4: the guest handler RETURNED */
    for (int r = 0; r < 29; r++) ss->__x[r] = g->uc_mcontext.regs[r];
    arm_thread_state64_set_fp(*ss, g->uc_mcontext.regs[29]);
    arm_thread_state64_set_lr_fptr(*ss, (void *)(uintptr_t)g->uc_mcontext.regs[30]);
    arm_thread_state64_set_sp(*ss, g->uc_mcontext.sp);
    arm_thread_state64_set_pc_fptr(*ss, (void *)(uintptr_t)g->uc_mcontext.pc);
    t_guest_uc_depth = depth;
}

static int is_fault_signal(int g) { return g == 4 || g == 5 || g == 6 || g == 7 || g == 8 || g == 11; }

/* Put THIS LAYER's dispatcher on a Darwin signal number, for the guest signal `sig` the guest armed. One place, because
 * the install has to be repeatable: the point of tl_signal_arm_guest is to install it again when something cleared it.
 * Returns 0 on success, else the host errno. */
static int host_arm(int d, int sig)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = host_signal_entry;
    sa.sa_flags = SA_SIGINFO;
    if (sig > 0 && sig < 32 && (g_guest_sig[sig].flags & 0x10000000)) sa.sa_flags |= SA_RESTART;
    sigemptyset(&sa.sa_mask);
    errno = 0;
    if (sigaction(d, &sa, NULL) == 0) return 0;
    return errno ? errno : EINVAL;
}

/*
 * ---------------------------------------------------------------- one-shot signal self-test (the discriminator)
 *
 * A stalled stop-the-world handshake (Boehm GC's, for il2cpp) leaves one question a log of counts cannot answer: can
 * THIS PROCESS deliver a THREAD-DIRECTED signal to one of its own threads at all, and if not, WHICH of the three
 * possible reasons is it -- (1) the mechanism is broken process-wide, (2) the Darwin number chosen for bionic SIGPWR is
 * one a thread-directed send will not run a handler for, or (3) the TARGET THREAD'S MASK blocks it? A send/run ledger
 * cannot separate them, and neither can a probe that only ever signals pthread_self(): a self-directed send runs on the
 * SENDER's mask, not on a target's, so it cannot speak to (3) at all.
 *
 * So the probe answers all three, by MEASUREMENT, at start-up, over FOUR Darwin signals and TWO directions:
 *
 *   Darwin 7  SIGEMT   the CONTROL -- the one Darwin number in 1..31 no bionic signal claims, so a clean delivery proves
 *                      the mechanism process-wide and a failure of it exonerates every other candidate
 *   Darwin 29 SIGINFO  what bionic SIGPWR (30), Boehm's stop-the-world signal, maps to. Darwin treats SIGINFO specially
 *                      (it is the ^T status request) but it IS catchable and a thread-directed send SHOULD run a handler;
 *                      this probe is the discriminator that measures it on the device instead of assuming it broken
 *   Darwin 24 SIGXCPU  bionic 24, the second signal il2cpp's collector arms
 *   Darwin 30 SIGUSR1  a known-ordinary, catchable signal -- a second control
 *
 * Each signal is tested SELF-DIRECTED (pthread_kill(pthread_self()) -- the sender's own mask applies) and CROSS-THREAD
 * (a real helper thread that EXPLICITLY sigemptyset/sigaddset/pthread_sigmask(SIG_UNBLOCK)s the signal, reads back and
 * REPORTS ITS OWN MASK, then observes whether the handler ran ON IT). Cross-thread with the target's mask cleared is
 * the shape that matters: it is what Boehm does, and a failure there while the control delivers cannot be blamed on the
 * mechanism or on the mask.
 *
 * Every send uses a BOUNDED wait -- a few sched_yield() boundaries, then 1 ms sleeps, capped -- never an unbounded one,
 * so a signal that never arrives cannot hang the start-up. One plain verdict line is printed per signal per direction.
 * Every disposition and mask the probe touches is restored. It also prints image identity (probe_image_identity) from
 * the RUNNING process, so a second copy of the layer -- the review's candidate 2 -- is visible in the same log.
 *
 * One shot, once, at start-up, on the layer's own entry thread before the guest arms anything; no lock, no allocation on
 * the handler path, nothing per-frame or per-call. Modeled on the JIT self-test's line so it reads the same way.
 */
/* The probe handler's OWN observation, per thread, plus the image counter. Both writes are async-signal-safe: a plain
 * __thread store and one relaxed atomic add. t_probe_seen is how a CROSS-THREAD probe's target answers "did MY handler
 * run" for itself, without any lock or log in the handler. */
static __thread volatile sig_atomic_t t_probe_seen;
static void probe_handler(int sig, siginfo_t *info, void *uc)
{
    (void)info; (void)uc;
    t_probe_seen = sig;
    tl_signal_probe_ran();
}

/* --- the mask MEASURED on the thread that owns it ---------------------------------------------------------------
 * No Darwin call reads ANOTHER thread's mask, so a mask reading is only ever valid when the thread reads its OWN. Both
 * probes therefore have the thread that owns the mask read it and hand the value back: the caller for a self-directed
 * send, the helper for a cross-thread one. `mask` = sigprocmask(SIG_SETMASK, NULL, &cur); `pending` = sigpending(), the
 * set of signals accepted for this thread but not yet delivered -- a signal that is BOTH blocked and pending is proof it
 * was accepted and swallowed by the mask, not discarded by a disposition. Returns 0 on success, -1 on failure. */
static int probe_own_mask(uint32_t *out)
{
    sigset_t cur;
    if (pthread_sigmask(SIG_SETMASK, NULL, &cur) != 0) return -1;
    memcpy(out, &cur, sizeof(*out));
    return 0;
}
static int probe_own_pending(uint32_t *out)
{
    sigset_t p;
    if (sigpending(&p) != 0) return -1;
    memcpy(out, &p, sizeof(*out));
    return 0;
}
static int probe_bit(uint32_t m, int dsig) { return dsig >= 1 && dsig <= 32 && (m & (1u << (dsig - 1))) != 0; }

/* One cross-thread probe: the helper thread's OWN mask (BEFORE and AFTER its own explicit unblock) and its OWN
 * observation, filled in BY the helper thread. */
typedef struct {
    int        dsig;
    uint32_t   mask_before;   /* the target's OWN mask BEFORE it unblocked: what it was BORN with */
    uint32_t   mask_after;    /* the target's OWN mask AFTER its explicit SIG_UNBLOCK */
    uint32_t   pend_after;    /* the target's OWN pending set after the bounded wait */
    int        mask_e_before, mask_e_after, pend_e, unblock_rc;   /* return codes, by the target */
    atomic_int go;            /* target -> the tester: I am running with the mask I just set */
    atomic_int done;          /* target -> the tester: I have made my observation */
    int        ran;           /* the target's OWN answer */
    char       name[32];
} probe_target;

/* Wait, BOUNDED (~`ms` at the outside), for a flag a helper thread sets: a few sched_yield boundaries, then 1 ms
 * sleeps. Never an unbounded wait -- a signal that never arrives must not hang the start-up. */
static void probe_wait(atomic_int *flag, int ms)
{
    for (int i = 0; i < ms; i++) {
        for (int y = 0; y < 16 && !atomic_load(flag); y++) sched_yield();
        if (atomic_load(flag)) return;
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
}

static void *probe_target_main(void *p)
{
    probe_target *t = p;
    pthread_getname_np(pthread_self(), t->name, sizeof(t->name));
    if (!t->name[0]) snprintf(t->name, sizeof(t->name), "signal-probe");
    t_probe_seen = 0;
    /* MEASURE BEFORE THE UNBLOCK: this is the mask the helper thread was BORN with (inherited from its creator at
     * pthread_create). Reading it here, on the thread that owns it, is the only way it can be read at all. */
    t->mask_e_before = probe_own_mask(&t->mask_before);
    sigset_t set;
    sigemptyset(&set); sigaddset(&set, t->dsig);
    t->unblock_rc = pthread_sigmask(SIG_UNBLOCK, &set, NULL);            /* the EXPLICIT target-side unblock */
    t->mask_e_after = probe_own_mask(&t->mask_after);                    /* and read the mask BACK, to prove it took */
    atomic_store(&t->go, 1);
    int seen = 0;                                                        /* its OWN bounded wait, ~200 ms */
    for (int i = 0; i < 200 && !seen; i++) {
        for (int y = 0; y < 16 && !seen; y++) { sched_yield(); seen = (t_probe_seen == t->dsig); }
        if (!seen) { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }
    }
    t->pend_e = probe_own_pending(&t->pend_after);   /* accepted-but-undelivered? then the mask swallowed it */
    t->ran = seen;
    atomic_store(&t->done, 1);
    return NULL;      /* after this store the helper touches nothing of the caller's: it is detached, never joined */
}

/* Send `dsig` thread-directed to the CALLING thread with the probe handler installed. The CALLER reads its OWN mask
 * BEFORE the send and again after, and its OWN pending set after the bounded wait -- the target-side measurement the
 * review asked for (a self-directed send runs on the sender's mask, so the sender IS the target here). *errno_out is
 * what pthread_kill returned (0 = accepted for the target); *ran_out is whether the probe handler actually ran.
 * Restores the disposition. Returns 0 if the probe could be installed and sent, -1 if the host refused the install. */
static int probe_deliver_self(int dsig, int *errno_out, int *ran_out,
                              uint32_t *mask_before_out, uint32_t *mask_after_out, uint32_t *pend_out)
{
    struct sigaction old, sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = probe_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    errno = 0;
    if (sigaction(dsig, &sa, &old) != 0) { *errno_out = errno ? errno : EINVAL; *ran_out = 0; return -1; }
    { char desc[320]; tl_signal_describe_disposition(dsig, desc, sizeof(desc));   /* READ THE PROBE INSTALL BACK */
      tl_log_line("signal-selftest: Darwin %d SELF-DIRECTED: the probe handler was installed with Darwin's own sigaction; the install reads back as: %s", dsig, desc); }
    if (mask_before_out) { *mask_before_out = 0; probe_own_mask(mask_before_out); }
    unsigned long before = tl_signal_probe_count();
    errno = 0;
    int dr = pthread_kill(pthread_self(), dsig);
    *errno_out = dr ? dr : (errno ? errno : 0);
    /* A self-directed signal is delivered on the way back to user mode -- usually already run. Give a deferred one a few
     * syscall boundaries before calling it absent. Bounded. */
    for (int i = 0; i < 8 && tl_signal_probe_count() == before; i++) sched_yield();
    *ran_out = tl_signal_probe_count() != before;
    if (mask_after_out) { *mask_after_out = 0; probe_own_mask(mask_after_out); }
    if (pend_out) { *pend_out = 0; probe_own_pending(pend_out); }
    sigaction(dsig, &old, NULL);
    return 0;
}

/* One cross-thread probe: install the handler on `dsig`, start a DETACHED helper that reports its OWN mask before and
 * after it explicitly unblocks `dsig`, send from HERE to that thread with pthread_kill, and let the helper say whether
 * ITS handler ran and whether the signal is still PENDING on it. *errno_out = what pthread_kill returned. Fills *t.
 * Restores the disposition. Returns -1 if the host refused the install or the helper could not start. */
static int probe_deliver_thread(int dsig, probe_target *t, int *errno_out)
{
    struct sigaction old, sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = probe_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    memset(t, 0, sizeof(*t));
    t->dsig = dsig;
    *errno_out = 0;
    errno = 0;
    if (sigaction(dsig, &sa, &old) != 0) { *errno_out = errno ? errno : EINVAL; return -1; }
    { char desc[320]; tl_signal_describe_disposition(dsig, desc, sizeof(desc));   /* READ THE INSTALL BACK before the send */
      tl_log_line("signal-selftest: Darwin %d CROSS-THREAD: the probe handler was installed with Darwin's own sigaction; the install reads back as: %s", dsig, desc); }
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512u << 10);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);   /* never joined: the app's threads are left alone */
    pthread_t th;
    int cr = pthread_create(&th, &at, probe_target_main, t);
    pthread_attr_destroy(&at);
    if (cr) { sigaction(dsig, &old, NULL); *errno_out = cr; return -1; }
    probe_wait(&t->go, 100);            /* bounded: the helper sets go before its own bounded wait */
    unsigned long before = tl_signal_probe_count();
    errno = 0;
    int dr = pthread_kill(th, dsig);
    *errno_out = dr ? dr : (errno ? errno : 0);
    probe_wait(&t->done, 300);          /* bounded, and LONGER than the helper's own 200 ms bound: it always reports */
    if (!atomic_load(&t->done) && tl_signal_probe_count() != before) t->ran = 1;   /* it ran but has not reported yet */
    sigaction(dsig, &old, NULL);
    return 0;
}

/* ---------------------------------------------------------------- disposition READ-BACK (the ledger's blind spot)
 * What the process ACTUALLY has installed on a Darwin signal, read with Darwin's own sigaction(d, NULL, &cur) -- the one
 * fact a send/run ledger cannot see, and the discriminator between the three ways "accepted but never ran" looks:
 *   reads back as SIG_DFL / SIG_IGN       -> Darwin DISCARDS a thread-directed send: no handler, nothing pending. This is
 *                                            the shape an install that was overwritten leaves, and the one the old ledger
 *                                            could not tell from a handler that simply never ran.
 *   reads back as ANOTHER IMAGE'S address -> something (a second copy of the layer, or host code that bypassed this layer)
 *                                            overwrote the install; THIS image's ledger can never count the runs.
 *   reads back as OUR host_signal_entry   -> the disposition is ours and correct, so the fault is the DELIVERY path (a
 *                                            tracer/exception port swallowing it), NOT the disposition.
 * Fills `out` with the address, its nearest symbol and its image (base), classified against this image's
 * host_signal_entry. Does the work only when called; callers gate it to once per signal. */
void tl_signal_describe_disposition(int d, char *out, size_t cap)
{
    struct sigaction cur;
    memset(&cur, 0, sizeof(cur));
    if (d <= 0) { snprintf(out, cap, "no Darwin number (this bionic signal has no host equivalent)"); return; }
    errno = 0;
    if (sigaction(d, NULL, &cur) != 0) {
        snprintf(out, cap, "UNREADABLE -- sigaction(Darwin %d, NULL, &cur) failed: %s (%d); the disposition cannot be read back here",
                 d, strerror(errno ? errno : EINVAL), errno ? errno : EINVAL);
        return;
    }
    void *addr = (cur.sa_flags & SA_SIGINFO) ? (void *)cur.sa_sigaction : (void *)cur.sa_handler;
    if (addr == (void *)SIG_DFL) {
        snprintf(out, cap, "SIG_DFL -- Darwin DISCARDS a thread-directed send of it: no handler runs and nothing is left pending");
        return;
    }
    if (addr == (void *)SIG_IGN) {
        snprintf(out, cap, "SIG_IGN -- Darwin DISCARDS a thread-directed send of it: no handler runs and nothing is left pending");
        return;
    }
    Dl_info di, me; memset(&di, 0, sizeof(di)); memset(&me, 0, sizeof(me));
    const char *sym = "?", *img = "?", *own_img = "?", *who;
    void *ibase = NULL;
    if (dladdr(addr, &di) && di.dli_fname) { img = di.dli_fname; ibase = di.dli_fbase; if (di.dli_sname) sym = di.dli_sname; }
    if (dladdr((void *)(uintptr_t)host_signal_entry, &me) && me.dli_fname) own_img = me.dli_fname;
    if (addr == (void *)(uintptr_t)host_signal_entry)
        who = "OURS (this image's host_signal_entry)";
    else if (me.dli_fbase && me.dli_fbase == ibase)
        who = "ANOTHER HANDLER in THIS image (not host_signal_entry)";
    else
        who = "ANOTHER IMAGE'S handler -- a different Mach-O installed it, so THIS image's ledger can never count its runs";
    snprintf(out, cap, "address %p (nearest symbol %s, image '%s' base %p) -- %s; this image's host_signal_entry at %p in '%s'",
             addr, sym, img, ibase, who, (void *)(uintptr_t)host_signal_entry, own_img);
}

/* ---------------------------------------------------------------- is a tracer attached? (the debugger hypothesis)
 * Reads P_TRACED from the kernel's own view of this process (sysctl CTL_KERN/KERN_PROC/KERN_PROC_PID). The layer's
 * signals are accepted (pthread_kill 0) and left neither pending nor delivered; the one mechanism OUTSIDE the process
 * that does exactly that is a debugger/tracer holding the task's exception ports. SET means a tracer is attached and can
 * intercept delivery; clear exonerates the tracer. One line, once. */
static void probe_ptraced(void)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    struct kinfo_proc kp; memset(&kp, 0, sizeof(kp));
    size_t len = sizeof(kp);
    errno = 0;
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0) {
        tl_log_line("signal-debugger: P_TRACED could not be read -- sysctl(KERN_PROC_PID, %d) failed: %s (%d)",
                    getpid(), strerror(errno ? errno : EINVAL), errno ? errno : EINVAL);
        return;
    }
    int traced = (kp.kp_proc.p_flag & P_TRACED) != 0;
    tl_log_line("signal-debugger: P_TRACED from sysctl(KERN_PROC_PID, %d): %s -- p_flag 0x%08x (a tracer attached here can hold the task's exception ports and swallow a thread-directed signal; P_TRACED clear exonerates the tracer)",
                getpid(), traced ? "SET (a debugger/tracer IS attached to this process)" : "clear (NO tracer attached)",
                kp.kp_proc.p_flag);
}

/* ---------------------------------------------------------------- RAW native delivery, every layer wrapper bypassed
 * The review's direct test of the debugger hypothesis: install a handler with DARWIN'S OWN sigaction (no layer shim), on
 * an ordinary Darwin signal, and raise() it and pthread_kill(pthread_self()) it with nothing of the layer in the path --
 * the send, the handler and the wait are all plain Darwin. If a raw raise of a raw handler does not run while the install
 * reads back as that handler (printed below), the delivery is intercepted outside the process (a tracer); if it DOES run,
 * the fault is the layer's own install/dispatch and the tracer is exonerated. Async-signal-safe: the handler is one
 * __thread store. The disposition is restored. */
static __thread volatile sig_atomic_t t_raw_seen;
static void raw_handler(int sig, siginfo_t *info, void *uc) { (void)info; (void)uc; t_raw_seen = sig; }

static void probe_raw_native_delivery(int dsig)
{
    struct sigaction old, sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = raw_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    errno = 0;
    if (sigaction(dsig, &sa, &old) != 0) {
        tl_log_line("signal-raw: Darwin %d: RAW sigaction install FAILED: %s (%d)", dsig, strerror(errno ? errno : EINVAL), errno ? errno : EINVAL);
        return;
    }
    { char desc[320]; tl_signal_describe_disposition(dsig, desc, sizeof(desc));   /* READ THE INSTALL BACK first */
      tl_log_line("signal-raw: Darwin %d: installed a RAW native handler (raw_handler at %p) with Darwin's own sigaction; the install reads back as: %s",
                  dsig, (void *)(uintptr_t)raw_handler, desc); }

    t_raw_seen = 0; errno = 0;                                    /* RAISE() -- a process-directed send */
    int rr = raise(dsig);
    for (int i = 0; i < 8 && !t_raw_seen; i++) sched_yield();
    tl_log_line("signal-raw: Darwin %d: RAW raise() returned %d (errno %s (%d)); the RAW native handler %s on this thread",
                dsig, rr, errno ? strerror(errno) : "0", errno, t_raw_seen ? "RAN" : "DID NOT RUN");

    t_raw_seen = 0; errno = 0;                                    /* PTHREAD_KILL(self) -- thread-directed, as the guest sends */
    int kr = pthread_kill(pthread_self(), dsig);
    for (int i = 0; i < 8 && !t_raw_seen; i++) sched_yield();
    tl_log_line("signal-raw: Darwin %d: RAW pthread_kill(pthread_self()) returned %d (errno %s (%d)); the RAW native handler %s on this thread",
                dsig, kr, errno ? strerror(errno) : "0", errno, t_raw_seen ? "RAN" : "DID NOT RUN");

    sigaction(dsig, &old, NULL);                                  /* restore */
}

/*
 * Image identity, from the RUNNING process (the review's candidate 2). The layer's own .c is compiled into the app
 * target's single PBXNativeTarget, so the build file says one image -- but a build file is not the process. If the layer
 * exists twice, signal dispositions are PROCESS-WIDE while g_sig / g_guest_sig / g_pwr_darwin are IMAGE-LOCAL: image B
 * can send a signal for which image A owns the installed handler, run the guest's handler, and still record zero runs in
 * its own ledger -- the send/run mismatch this whole investigation turns on, and no probe can see it from one image
 * alone. So this prints, once, where every part of the signal subsystem ACTUALLY lives: the dladdr() image (path and
 * base) of each signal-subsystem function and global, and a scan of every loaded image for a second copy of the layer.
 */
static void probe_image_identity(void)
{
    struct { const char *what; void *addr; } sym[] = {
        { "host_signal_entry",          (void *)(uintptr_t)host_signal_entry },
        { "host_arm",                   (void *)(uintptr_t)host_arm },
        { "tl_signal_arm_guest",        (void *)(uintptr_t)tl_signal_arm_guest },
        { "&b_kill (the send site)",    (void *)(uintptr_t)tl_signal_send_site() },
        { "&g_sig (the ledger)",        tl_signal_ledger_image() },
        { "&g_guest_sig (handler tab)", tl_signal_guest_table_image() },
        { "&g_pwr_darwin",              tl_signal_pwr_addr() },
    };
    const void *layer_base = NULL; char layer_path[512] = "?";
    for (size_t i = 0; i < sizeof(sym) / sizeof(sym[0]); i++) {
        Dl_info di; memset(&di, 0, sizeof(di));
        if (sym[i].addr && dladdr(sym[i].addr, &di) && di.dli_fname) {
            tl_log_line("signal-image: %-27s at %p: image '%s' base %p%s%s", sym[i].what, sym[i].addr,
                        di.dli_fname, di.dli_fbase,
                        di.dli_sname ? ", nearest symbol " : "", di.dli_sname ? di.dli_sname : "");
            if (!layer_base) { layer_base = di.dli_fbase; snprintf(layer_path, sizeof(layer_path), "%s", di.dli_fname); }
            else if (di.dli_fbase != layer_base)
                tl_log_line("signal-image: *** %s is in a DIFFERENT IMAGE (%p '%s') from %s (%p '%s') -- the signal "
                            "subsystem is SPLIT ACROSS TWO IMAGES, and dispositions are process-wide while its state is "
                            "not ***", sym[i].what, di.dli_fbase, di.dli_fname, sym[0].what, layer_base, layer_path);
        } else {
            tl_log_line("signal-image: %-27s at %p: dladdr() could not name the image", sym[i].what, sym[i].addr);
        }
    }
    uint32_t n = _dyld_image_count();
    const char *exe = _dyld_get_image_name(0);
    const char *slash = exe ? strrchr(exe, '/') : NULL;
    const char *exe_base = slash ? slash + 1 : (exe ? exe : "");
    /* The matching rule is the fix. The previous loop started at i == 0 and compared the main executable's OWN entry
     * against itself: `!strcmp(nm, exe)` was trivially true for image 0, so "a second copy of the app executable" read
     * YES on EVERY run -- a false positive of the rule, not a measured duplicate. This scan SKIPS i == 0 and PRINTS every
     * entry it counts, verbatim (path + base + index + which rule matched), so the next run either names the real second
     * Mach-O or proves there is none. */
    int dup_exe_path = 0, dup_exe_name = 0, dup_path = 0, staged = 0;
    for (uint32_t i = 0; i < n; i++) {
        const char *nm = _dyld_get_image_name(i);
        if (!nm) continue;
        const void *hdr = _dyld_get_image_header(i);
        const char *s2 = strrchr(nm, '/');
        const char *base = s2 ? s2 + 1 : nm;
        if (strstr(nm, "TranslationLayer")) { staged++; tl_log_line("signal-image: a STAGED translation layer image is loaded: '%s' base %p", nm, hdr); }
        if (i == 0) continue;                       /* the main executable is not a "second copy" of itself */
        if (exe && !strcmp(nm, exe)) {              /* (a) the SAME path loaded a second time */
            dup_exe_path++;
            tl_log_line("signal-image: DUPLICATE (rule a: SAME PATH as the main executable, listed twice) #%d: '%s' base %p (index %u)", dup_exe_path, nm, hdr, i);
            continue;
        }
        if (*exe_base && !strcmp(base, exe_base) && exe && strcmp(nm, exe)) {   /* (b) same BASENAME, different path */
            dup_exe_name++;
            tl_log_line("signal-image: DUPLICATE (rule b: same BASENAME '%s' as the main executable, DIFFERENT path) #%d: '%s' base %p (index %u)", exe_base, dup_exe_name, nm, hdr, i);
            continue;
        }
        for (uint32_t j = i + 1; j < n; j++) {      /* (c) any path that appears more than once anywhere in the list */
            const char *nj = _dyld_get_image_name(j);
            if (nj && !strcmp(nj, nm)) {
                dup_path++;
                tl_log_line("signal-image: DUPLICATE (rule c: this path appears more than once in the image list) #%d: '%s' base %p (index %u, again at index %u)", dup_path, nm, hdr, i, j);
                break;
            }
        }
    }
    int dup_exe = dup_exe_path + dup_exe_name;
    tl_log_line("signal-image: %u image(s) loaded; the layer's signal code lives in '%s' base %p; a second copy of the app "
                "executable: %s (same-path duplicates %d, same-basename duplicates %d, repeated paths %d); a staged TranslationLayer image: %s -- %s",
                n, layer_path, layer_base, dup_exe ? "YES" : "none", dup_exe_path, dup_exe_name, dup_path, staged ? "YES" : "none",
                (dup_exe || staged) ? "*** TWO-IMAGE HAZARD: the signal subsystem is not a single owner here -- one copy "
                                      "can own the disposition while another counts the runs ***"
                                    : "ONE image owns the signal subsystem (candidate 2 is refuted on this run; every count above is printed, not inferred)");
}

/* ================================================================== Mach signal delivery
 *
 * WHY THIS EXISTS (measured, build 3803577): an attached JIT debugger (StikDebug) intercepts and DISCARDS Unix signals
 * before they reach this process's own sigaction handlers. A raw native handler installed with Darwin's own sigaction,
 * read back as installed, still does not RUN on a raw raise()/pthread_kill(self) -- while P_TRACED is set. So the guest's
 * cross-thread signals (il2cpp's Boehm GC stop-the-world sends bionic SIGPWR 30 -> Darwin SIGINFO 29 to every managed
 * thread) are ACCEPTED by pthread_kill and then vanish: the suspend handler never runs, the collector's wait-to-stop loop
 * (UnityMain in usleep) retries for ever, and the engine never reaches its frame loop. The send side, the install side and
 * the mask are all correct and exonerated by measurement; the delivery is intercepted outside the process.
 *
 * THE FIX: deliver the guest's handler to the target thread by Mach thread control instead of an OS signal -- the debugger
 * swallows signals, not thread_set_state. On a guest send we thread_suspend the target, capture its registers, redirect it
 * to mach_sig_dispatch on its OWN stack (so the guest handler sees the target's real stack pointer, which is what Boehm
 * scans), and thread_resume. mach_sig_dispatch runs the guest handler through the SAME host_signal_entry a real signal
 * would use (so the ledger and the synthesized bionic ucontext are identical), then setcontext()s back to where the target
 * was -- setcontext is Darwin's own restore and matches the arm_thread_state64_* accessor's PAC signing.
 *
 * SAFETY: every Mach call is checked; any failure returns -1 and the caller falls back to a real pthread_kill (the current
 * behaviour) rather than guessing. A one-shot SELF-TEST at start-up proves the whole round-trip (redirect -> handler ->
 * setcontext -> the target continues) on THIS device before the mechanism is trusted with a GC; if it does not round-trip,
 * g_mach_ok is 0 and every send falls back. This is the narrow, guest-requested suspend the owner approved: it is the app
 * asking to stop ITS OWN threads for a GC, not the layer killing or joining anyone.
 */
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_act.h>

static volatile int g_mach_ok = -1;                 /* -1 untested, 0 FAILED (fall back), 1 round-trips here */
static volatile sig_atomic_t g_mach_test_ran;
static volatile sig_atomic_t g_mach_dispatch_ran;   /* did the redirected thread actually run mach_sig_dispatch? */
static volatile int g_mach_test_done;

/* Which target threads are CURRENTLY inside mach_sig_dispatch (parked in the guest handler it ran). Measured: Boehm's
 * suspend handler parks the thread by design (it waits for the world to restart), so the thread stays inside the dispatch.
 * A second signal to that same thread (the collector's restart) MUST NOT re-redirect it -- a nested redirect overwrites the
 * park frame and wedges the thread at mach_sig_dispatch+0, which is exactly the freeze. It is woken instead. */
#define MACH_PARK_MAX 64
static pthread_t g_mach_park[MACH_PARK_MAX];
static volatile int g_mach_delivered_direct, g_mach_woke;
static int mach_park_has(pthread_t t) { for (int i = 0; i < MACH_PARK_MAX; i++) if (g_mach_park[i] == t) return 1; return 0; }
static void mach_park_add(pthread_t t) { for (int i = 0; i < MACH_PARK_MAX; i++) if (!g_mach_park[i]) { g_mach_park[i] = t; return; } }
static void mach_park_del(pthread_t t) { for (int i = 0; i < MACH_PARK_MAX; i++) if (g_mach_park[i] == t) g_mach_park[i] = NULL; }

static void mach_test_handler(int gsig, siginfo_t *info, void *uc)
{ (void)gsig; (void)info; (void)uc; g_mach_test_ran++; }

/* Resume a thread at a captured __darwin_arm_thread_state64. Naked: restore x0-x30, sp, then branch to the pc (already
 * PAC-stripped by arm_thread_state64_get_pc). x16/x17 are AAPCS64 scratch (IP0/IP1) and are deliberately not restored --
 * the ABI does not preserve them across a call, so a handler may clobber them exactly as a real signal's return may. */
__attribute__((naked, used)) static void mach_sig_restore(struct __darwin_arm_thread_state64 *ss, uintptr_t pc)
{
    __asm__ volatile(
        "mov x16, x0\n"
        "mov x17, x1\n"
        "ldp x0,  x1,  [x16, #0]\n"
        "ldp x2,  x3,  [x16, #16]\n"
        "ldp x4,  x5,  [x16, #32]\n"
        "ldp x6,  x7,  [x16, #48]\n"
        "ldp x8,  x9,  [x16, #64]\n"
        "ldp x10, x11, [x16, #80]\n"
        "ldp x12, x13, [x16, #96]\n"
        "ldp x14, x15, [x16, #112]\n"
        "ldp x18, x19, [x16, #144]\n"
        "ldp x20, x21, [x16, #160]\n"
        "ldp x22, x23, [x16, #176]\n"
        "ldp x24, x25, [x16, #192]\n"
        "ldp x26, x27, [x16, #208]\n"
        "ldr x28,      [x16, #224]\n"
        "ldp x29, x30, [x16, #232]\n"
        "ldr x16,      [x16, #248]\n"
        "mov sp, x16\n"
        "br x17\n");
}

/* Runs ON the redirected target thread. Same dispatcher a real signal reaches, then resume exactly where the target was. */
static void mach_sig_dispatch(int gsig, siginfo_t *info, ucontext_t *uc)
{
    mcontext_t mc = uc->uc_mcontext;
    g_mach_dispatch_ran++;
    host_signal_entry(tl_signal_to_darwin(gsig), info, uc);
    mach_park_del(pthread_self());   /* the handler returned: this thread is no longer parked inside a dispatch */
    uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(mc->__ss);
    mach_sig_restore(&mc->__ss, pc);
    __builtin_unreachable();
}

/* Deliver guest signal `gsig` to `target` by Mach thread control. 0 = delivered; -1 = caller should pthread_kill. */
static int mach_sig_deliver(pthread_t target, int gsig)
{
    mach_port_t tp = pthread_mach_thread_np(target);
    if (!tp) return -1;
    /* Boehm's suspend handler parks in sigsuspend by design, so a target may already be inside a dispatch. A WAKE alone is
     * not enough: the guest installed a handler for the restart signal, and a real kernel delivers it NESTED on this
     * thread. Measured on device (d259bb2): signal 24 sent 2, handler ran 0, target still in __sigsuspend, the world never
     * restarted. So deliver nested -- but refuse the one redirect that provably loops: a thread already sitting ON the
     * dispatcher's entry (the old mach_sig_dispatch+0 wedge). */
    int nested = mach_park_has(target);
    if (thread_suspend(tp) != KERN_SUCCESS) return -1;
    /* A thread parked in an interruptible syscall (nanosleep, futex, poll -- where a guest thread waits) keeps its PC in
     * the kernel; a thread_set_state PC change is then lost when the syscall returns. thread_abort_safely brings it back to
     * a safe point in user space first (KERN_FAILURE simply means it was not in a syscall, so nothing to abort). */
    thread_abort_safely(tp);
    struct __darwin_arm_thread_state64 st;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(tp, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) != KERN_SUCCESS) {
        thread_resume(tp); return -1;
    }
    if (nested) {
        uintptr_t cur = (uintptr_t)arm_thread_state64_get_pc(st);
        uintptr_t disp = (uintptr_t)(void (*)(void))mach_sig_dispatch;
        if (g_mach_woke++ == 0)
            tl_log_line("signal-mach: delivering NESTED to a target parked in a dispatch (a wake alone never ran the handler: "
                        "measured sent 2, ran 0) -- the restart handler must run on the parked thread");
        if (cur == disp) {   /* already on the dispatcher's entry: one more redirect loops at +0 forever */
            thread_resume(tp);
            return 1;
        }
    }
    /* Carve ucontext + siginfo + mcontext on the target's OWN stack, below its SP (16-byte aligned), so the guest handler
     * sees the target's real stack pointer and mach_sig_restore returns it there. The dispatch runs from base up into this. */
    uintptr_t sp = ((uintptr_t)st.__sp) & ~15ull;
    size_t need = sizeof(ucontext_t) + sizeof(siginfo_t) + 4096 + 512;
    uintptr_t base = (sp - need) & ~15ull;
    ucontext_t *uc = (ucontext_t *)base;
    siginfo_t *info = (siginfo_t *)(base + sizeof(ucontext_t));
    mcontext_t mc = (mcontext_t)(base + sizeof(ucontext_t) + sizeof(siginfo_t));
    memset(uc, 0, sizeof(*uc)); memset(info, 0, sizeof(*info)); memset(mc, 0, 4096);
    uc->uc_mcontext = mc;
    mc->__ss = st;                                 /* the target's registers: what the handler reads and we restore */
    struct __darwin_arm_thread_state64 nst = st;
    arm_thread_state64_set_pc_fptr(nst, (void *)(uintptr_t)mach_sig_dispatch);
    arm_thread_state64_set_sp(nst, base);
    nst.__x[0] = (uint64_t)gsig;
    nst.__x[1] = (uint64_t)(uintptr_t)info;
    nst.__x[2] = (uint64_t)(uintptr_t)uc;
    mach_park_add(target);                     /* it will be inside the dispatch until its handler returns (or parks) */
    if (thread_set_state(tp, ARM_THREAD_STATE64, (thread_state_t)&nst, ARM_THREAD_STATE64_COUNT) != KERN_SUCCESS) {
        mach_park_del(target); thread_resume(tp); return -1;
    }
    thread_resume(tp);
    g_mach_delivered_direct++;
    return 0;
}

static void *mach_test_thread(void *arg)
{
    (void)arg;
    while (!g_mach_test_done) { struct timespec ts = { 0, 2000000 }; nanosleep(&ts, NULL); }
    return NULL;
}

/* One-shot: prove the redirect -> handler -> setcontext -> continue round-trip on THIS device before trusting it with a GC. */
static void tl_signal_mach_selftest(void)
{
    g_mach_test_ran = 0; g_mach_test_done = 0; g_mach_dispatch_ran = 0;
    int T = 21;                                    /* an unused bionic signal (the collector uses 30/24) */
    void *sh = g_guest_sig[T].handler; int sf = g_guest_sig[T].flags;
    g_guest_sig[T].handler = (void *)(uintptr_t)mach_test_handler; g_guest_sig[T].flags = 0;
    pthread_t th;
    if (pthread_create(&th, NULL, mach_test_thread, NULL) != 0) {
        g_guest_sig[T].handler = sh; g_guest_sig[T].flags = sf;
        g_mach_ok = 0;
        tl_log_line("signal-mach: selftest FAIL -- could not start the test thread; falling back to pthread_kill");
        return;
    }
    struct timespec w = { 0, 20000000 }; nanosleep(&w, NULL);
    int delivered = mach_sig_deliver(th, T);
    for (int i = 0; i < 250 && !g_mach_test_ran; i++) { struct timespec s = { 0, 2000000 }; nanosleep(&s, NULL); }
    g_mach_test_done = 1; pthread_join(th, NULL);
    g_guest_sig[T].handler = sh; g_guest_sig[T].flags = sf;
    g_mach_ok = (delivered == 0 && g_mach_test_ran > 0) ? 1 : 0;
    tl_log_line("signal-mach: selftest %s (deliver=%d, dispatch ran %d time(s), handler ran %d time(s)) -- Mach thread-control "
                "delivery of a guest signal %s on this device, so a guest stop-the-world signal will %s be delivered despite the tracer",
                g_mach_ok ? "PASS" : "FAIL", delivered, (int)g_mach_dispatch_ran, (int)g_mach_test_ran,
                g_mach_ok ? "round-trips (redirect -> handler -> restore -> the target continued)" : "does NOT round-trip",
                g_mach_ok ? "now" : "NOT (every send falls back to pthread_kill)");
}

/* The send path calls this first. 0 = delivered by Mach; -1 = the caller must fall back to a real pthread_kill. */
int tl_signal_deliver(pthread_t target, int gsig)
{
    if (getenv("TL_NO_MACH_SIGNAL")) return -1;    /* kill-switch: force the real-signal path */
    if (g_mach_ok == 0) return -1;                 /* measured broken here: never risk the redirect */
    if (g_mach_ok == 1) return mach_sig_deliver(target, gsig);
    return -1;                                     /* untested: fall back (the self-test has not run yet) */
}
#else
int tl_signal_deliver(pthread_t target, int gsig) { (void)target; (void)gsig; return -1; }
#endif /* __APPLE__ */

void tl_signal_selftest(void)
{
    static int done;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_SEQ_CST)) return;   /* once, whatever thread reaches here first */
    void *img = tl_signal_ledger_image();

    /* Where every part of the signal subsystem actually lives, printed once, BEFORE any measurement is read. */
    probe_image_identity();

    /* The two DIRECT tests the review asked for, both before any layer shim is involved: is a tracer attached, and does
     * a RAW native handler (Darwin's own sigaction, raise(), pthread_kill(self)) run at all with EVERY layer wrapper
     * bypassed? If the raw handler does not run while its install reads back as itself, the delivery is intercepted
     * outside the process (the tracer); if it does run, the layer's own install/dispatch is the fault. */
    probe_ptraced();
    probe_raw_native_delivery(SIGUSR2);
    probe_raw_native_delivery(SIGUSR1);

    char self_name[32] = ""; pthread_getname_np(pthread_self(), self_name, sizeof(self_name));
    if (!self_name[0]) snprintf(self_name, sizeof(self_name), "?");

    /* FOUR Darwin signals, TWO directions. SIGEMT is the control (a number no bionic signal claims); SIGINFO is what
     * bionic SIGPWR (30) maps to -- the discriminator; SIGXCPU is bionic 24; SIGUSR1 is a known-ordinary catchable signal. */
    static const int   cand[4]      = { SIGEMT,  SIGINFO, SIGXCPU, SIGUSR1 };
    static const char *cand_name[4] = { "SIGEMT (control)", "SIGINFO", "SIGXCPU", "SIGUSR1 (control)" };
    int          self_ok[4],  self_e[4],  self_ran[4],  self_unb[4];
    uint32_t     self_mb[4],  self_ma[4],  self_pd[4];
    int          cross_ok[4], cross_e[4];
    probe_target ct[4];

    /* THE CALLING THREAD'S OWN MASK, MEASURED FIRST -- before anything is sent. The self-test runs on a thread the LAYER
     * started, and a self-directed send is delivered on the SENDER's mask, so this one reading is what decides whether a
     * "pthread_kill returned 0 but the handler never ran" is the mask or something else. It is a reading, not an
     * inference: no Darwin call can read another thread's mask, but a thread can always read its own. */
    {
        uint32_t own = 0; int e = probe_own_mask(&own);
        tl_log_line("signal-selftest: the thread this test runs on ('%s') READ ITS OWN mask: 0x%08x -- %s", self_name, own,
                    e != 0 ? "the read FAILED (no mask could be read here)"
                           : (own ? "SIGNALS ARE BLOCKED on this thread; a self-directed send to it is left PENDING"
                                  : "empty: nothing is blocked, so a self-directed send that does not run a handler is NOT a mask"));
    }

    for (int i = 0; i < 4; i++) {
        /* The caller's own mask BEFORE anything is applied, then an EXPLICIT unblock ON THIS THREAD, then the send: the
         * before/after pair the review asked for, taken on the thread that owns the mask. */
        self_mb[i] = 0; probe_own_mask(&self_mb[i]);
        sigset_t us; sigemptyset(&us); sigaddset(&us, cand[i]);
        errno = 0;
        int ur = pthread_sigmask(SIG_UNBLOCK, &us, NULL);
        self_unb[i] = ur ? (errno ? errno : ur) : 0;
        self_ok[i]  = probe_deliver_self(cand[i], &self_e[i], &self_ran[i], NULL, &self_ma[i], &self_pd[i]) == 0;
        cross_ok[i] = probe_deliver_thread(cand[i], &ct[i], &cross_e[i]) == 0;
    }

    /* One measured line per signal per direction. Each carries the mask the OWNER of that thread read for itself, BEFORE
     * and AFTER its explicit unblock, its own PENDING set after the bounded wait, pthread_kill's return, and whether the
     * probe handler actually ran THERE. The pending set is what separates the two ways an accepted signal can vanish: a
     * signal that is BLOCKED is left PENDING (the mask swallowed it), while one whose disposition was cleared or whose
     * delivery was intercepted is neither blocked nor pending (the send returned 0 and DISCARDED it). */
    for (int i = 0; i < 4; i++) {
        const char *self_why = self_ran[i] ? "it was DELIVERED, so this thread's mask did not swallow it"
            : probe_bit(self_ma[i], cand[i]) ? "STILL BLOCKED after the unblock and STILL PENDING: the MASK is the cause"
            : probe_bit(self_pd[i], cand[i]) ? "not blocked but LEFT PENDING: something other than the mask is holding it"
            : "NOT blocked and NOT pending: the mask is NOT the cause -- a send that returns 0 and leaves nothing pending was DISCARDED, so look at the disposition or at a delivery intercepted outside the process";
        if (!self_ok[i])
            tl_log_line("signal-selftest: Darwin %-2d %-17s SELF-DIRECTED (%s): the host refused the probe handler: %s (%d)",
                        cand[i], cand_name[i], self_name, strerror(self_e[i]), self_e[i]);
        else
            tl_log_line("signal-selftest: Darwin %-2d %-17s SELF-DIRECTED (%s): OWN mask BEFORE 0x%08x (Darwin %d %s); explicit "
                        "SIG_UNBLOCK on this thread returned %d -> mask AFTER 0x%08x (Darwin %d %s); pthread_kill returned %s (%d); "
                        "the probe handler %s -- %s",
                        cand[i], cand_name[i], self_name, self_mb[i], cand[i],
                        probe_bit(self_mb[i], cand[i]) ? "BLOCKED" : "clear", self_unb[i], self_ma[i], cand[i],
                        probe_bit(self_ma[i], cand[i]) ? "STILL BLOCKED" : "clear",
                        self_e[i] ? strerror(self_e[i]) : "0 = accepted", self_e[i],
                        self_ran[i] ? "RAN" : "DID NOT RUN", self_why);
        if (!cross_ok[i])
            tl_log_line("signal-selftest: Darwin %-2d %-17s CROSS-THREAD: could not probe -- the host refused the install or "
                        "the helper thread would not start: %s (%d)",
                        cand[i], cand_name[i], cross_e[i] ? strerror(cross_e[i]) : "-", cross_e[i]);
        else {
            const char *xwhy = ct[i].ran ? "it ran on THAT thread, so that thread can receive it"
                : probe_bit(ct[i].mask_after, cand[i]) ? "the target is STILL BLOCKED in it after its own unblock: the MASK is the cause"
                : probe_bit(ct[i].pend_after, cand[i]) ? "not blocked on the target but LEFT PENDING there: something other than the mask holds it"
                : "NOT blocked and NOT pending on the target: the mask is NOT the cause, so the number or the delivery, not the mask, is the discriminator";
            tl_log_line("signal-selftest: Darwin %-2d %-17s CROSS-THREAD to '%s': the target READ ITS OWN mask: BEFORE its unblock "
                        "0x%08x (Darwin %d %s, i.e. what it was BORN with), SIG_UNBLOCK returned %d, AFTER 0x%08x (Darwin %d %s), its "
                        "pending set after the wait 0x%08x; pthread_kill returned %s (%d); the probe handler %s on THAT thread -- %s",
                        cand[i], cand_name[i], ct[i].name[0] ? ct[i].name : "?", ct[i].mask_before, cand[i],
                        probe_bit(ct[i].mask_before, cand[i]) ? "BLOCKED" : "clear", ct[i].unblock_rc, ct[i].mask_after, cand[i],
                        probe_bit(ct[i].mask_after, cand[i]) ? "STILL BLOCKED" : "clear", ct[i].pend_after,
                        cross_e[i] ? strerror(cross_e[i]) : "0 = accepted", cross_e[i],
                        ct[i].ran ? "RAN" : "DID NOT RUN", xwhy);
        }
    }

    /* The verdicts the probe exists for. The CONTROL answers "can this process run a handler on another thread at all";
     * SIGINFO's line answers whether the number bionic SIGPWR is mapped to is deliverable cross-thread. */
    int ctrl_cross = cross_ok[0] && ct[0].ran;   /* SIGEMT: no bionic signal claims it */
    int info_cross = cross_ok[1] && ct[1].ran;   /* SIGINFO: what bionic SIGPWR (30) maps to */
    if (!cross_ok[0])
        tl_log_line("signal-selftest: VERDICT -- the control (Darwin %d/SIGEMT) could not even be installed or sent: this "
                    "process's ability to deliver a thread-directed signal is UNTESTED, not absent", SIGEMT);
    else if (ctrl_cross)
        tl_log_line("signal-selftest: VERDICT -- the control (Darwin %d/SIGEMT) ran the probe handler on ANOTHER thread: this "
                    "process CAN deliver a thread-directed signal cross-thread, so a guest stop-the-world handshake is "
                    "POSSIBLE here and a missing handler run is about the NUMBER or the TARGET, never the mechanism", SIGEMT);
    else
        tl_log_line("signal-selftest: VERDICT -- the control (Darwin %d/SIGEMT) did NOT run a handler on another thread while "
                    "its disposition was installed and the target unblocked it: this process can NOT complete a cross-thread "
                    "signal handshake AT ALL -- that is the fault to fix, and it makes the Darwin number and the target's mask "
                    "both beside the point", SIGEMT);
    /* The mask-vs-mechanism verdict, from the CONTROL's own readings. The control (Darwin 7/SIGEMT) is a number no bionic
     * signal claims, and its mask was read by the thread that owns it -- so this line is a measurement, never an inference.
     * It separates the three ways an accepted, un-run signal can look: still blocked (the mask), clear but pending (held
     * outside the mask), or clear and not pending (discarded: the disposition, not the mask). */
    if (self_ok[0]) {
        int blk_after = probe_bit(self_ma[0], SIGEMT);
        if (self_ran[0])
            tl_log_line("signal-selftest: VERDICT -- the control (Darwin %d/SIGEMT) RAN a SELF-DIRECTED handler on '%s': the calling "
                        "thread's OWN mask was 0x%08x (Darwin %d %s) and the explicit unblock made it 0x%08x, so a self-directed "
                        "delivery works here and a missing handler run is about the TARGET's mask, not this thread's",
                        SIGEMT, self_name, self_mb[0], SIGEMT,
                        probe_bit(self_mb[0], SIGEMT) ? "BLOCKED" : "clear", self_ma[0]);
        else if (blk_after)
            tl_log_line("signal-selftest: VERDICT -- the control (Darwin %d/SIGEMT) did NOT run and the calling thread's OWN mask STILL "
                        "had Darwin %d BLOCKED AFTER an explicit SIG_UNBLOCK (0x%08x -> 0x%08x): the MASK is the cause of the missing "
                        "handler, and a stop-the-world handshake cannot complete until that mask is cleared at the ROOT (the thread "
                        "was born with it by inheritance from HOST code)", SIGEMT, SIGEMT, self_mb[0], self_ma[0]);
        else if (probe_bit(self_pd[0], SIGEMT))
            tl_log_line("signal-selftest: VERDICT -- the control (Darwin %d/SIGEMT) did NOT run, its mask is CLEAR and the signal is "
                        "LEFT PENDING (0x%08x): something OTHER than this thread's mask is holding the delivery -- an exception port or "
                        "a tracer intercepting it. That is not a mask and cannot be cleared by a mask call", SIGEMT, self_pd[0]);
        else
            tl_log_line("signal-selftest: VERDICT -- the control (Darwin %d/SIGEMT) did NOT run, its mask is CLEAR, and the signal is NOT "
                        "pending (pending set 0x%08x): the send returned 0 and DISCARDED it. The mask is EXONERATED on this thread -- the "
                        "fault is the disposition or the delivery, not a blocked signal; look at the Darwin number, the disposition, and "
                        "anything outside the process that intercepts signals", SIGEMT, self_pd[0]);
    }
    if (cross_ok[1])
        tl_log_line("signal-selftest: VERDICT -- Darwin %d/SIGINFO (what bionic SIGPWR 30 maps to) %s the probe handler on the "
                    "target: Darwin SIGINFO IS %s by a thread-directed pthread_kill here",
                    SIGINFO, ct[1].ran ? "RAN" : "DID NOT RUN",
                    ct[1].ran ? "deliverable, so the SIGPWR->SIGINFO mapping is NOT the defect on this run (the review's reading "
                                "is confirmed on the device)" : "NOT delivered, so the NUMBER is the discriminator in this build");

    /* Decide from the MEASUREMENT only: move bionic SIGPWR off Darwin 29 when the control delivers cross-thread and SIGINFO
     * does not. Never on a guess -- the review established SIGINFO is catchable, so a run where SIGINFO delivers never
     * takes this branch. */
    int remap = cross_ok[0] && ctrl_cross && !info_cross;
    if (remap) {
        tl_signal_set_pwr_darwin(SIGEMT);
        tl_log_line("signal-selftest: bionic SIGPWR (30) was mapped to Darwin %d (SIGINFO), which a thread-directed send to "
                    "another thread did NOT run a handler for (%s), while Darwin %d (SIGEMT) did: for this run bionic SIGPWR "
                    "uses Darwin %d -- a number no bionic signal claims, so no two share a number and Darwin 29 is freed",
                    SIGINFO, cross_ok[1] ? "the probe handler did not run" : "the host would not install a probe", SIGEMT, SIGEMT);
    }
    /* The mapping's own contract, checked rather than assumed: no two bionic signals on one Darwin number after the choice. */
    int collision = 0, ca = 0, cb = 0, cd = 0;
    for (int a = 1; a < 32 && !collision; a++) {
        int da = tl_signal_to_darwin(a);
        if (da <= 0) continue;
        for (int b = a + 1; b < 32; b++) if (tl_signal_to_darwin(b) == da) { collision = 1; ca = a; cb = b; cd = da; break; }
    }
    tl_log_line("signal-selftest: bionic SIGPWR (30) uses Darwin %d this run (%s); mapping %s",
                tl_signal_to_darwin(30), tl_signal_pwr_darwin() ? "moved off SIGINFO by the measurement above" : "the table default, SIGINFO",
                collision ? "COLLIDES: two bionic signals share a Darwin number" : "checked -- no two bionic signals share a Darwin number");
    if (collision)
        tl_log_line("signal-selftest: COLLISION -- bionic %d and bionic %d both map to Darwin %d, which would run the wrong guest handler", ca, cb, cd);

    /* The two-image hazard, in one line: the probe's OWN run count and the ledger address of the image that installed it. */
    tl_log_line("signal-selftest: the probe handler ran %lu time(s) as recorded in THIS image's ledger (ledger image %p); the "
                "stall/close report prints the same count and address, so a layer loaded twice -- a handler in one image, the "
                "report in another -- shows them disagreeing",
                tl_signal_probe_count(), img);

    /* Now prove the Mach delivery round-trip (redirect -> handler -> setcontext -> the target continues) on this device,
     * so the send path knows whether to deliver a guest signal by thread control or to fall back to pthread_kill. */
    tl_signal_mach_selftest();
}

/*
 * Before a guest-to-guest signal is SENT, make sure the guest's own dispatcher is really installed on the Darwin
 * signal the send is about to use, and install it again if it is not.
 *
 * Why this is load-bearing, not belt-and-braces: Darwin DISCARDS a signal whose disposition is SIG_DFL or SIG_IGN. It
 * runs no handler and it leaves nothing pending, so a send of a signal whose disposition was cleared and a send of a
 * signal whose handler never ran are INDISTINGUISHABLE in the ledger (both read as "sent N, RAN 0, refused 0"). Anything
 * that puts SIG_DFL back -- a runtime restoring an old action, a crash reporter chain, a teardown -- turns every
 * subsequent send into a silent drop, which is exactly how a collector that stops the world by signalling its threads
 * hangs for ever with 211 sends and not one handler run. Re-arming costs one sigaction() query on the send path only
 * (a signal is sent once per suspended thread per collection; never per frame, never per call), and it is recorded in
 * the ledger so the run that needed it says so.
 *
 * The guest's handler for `gsig` is remembered by b_sigaction; if the guest armed nothing (or asked for SIG_IGN) there
 * there is nothing to arm and 0 is returned. -1 means the host refused the install (recorded with the errno).
 */
static int g_disp_said[32];       /* one read-back report per Darwin number (see tl_signal_describe_disposition) */
int tl_signal_arm_guest(int d, int gsig)
{
    if (d <= 0 || gsig < 1 || gsig > 31) return 0;
    void *h = g_guest_sig[gsig].handler;
    if (!h || h == (void *)1) return 0;
    struct sigaction cur;
    memset(&cur, 0, sizeof(cur));
    if (sigaction(d, NULL, &cur) != 0) return 0;                 /* cannot read it: do not guess */
    /* SAY WHAT THE DISPOSITION ACTUALLY IS, once per Darwin number, before deciding anything. This is the fact the
     * ledger could never see: SIG_DFL/SIG_IGN means the send will be DISCARd; another image's address means the install
     * was overwritten. Gated by a plain int so it pays nothing on the send path after the first time per signal. */
    if (d < 32 && !g_disp_said[d]) {
        g_disp_said[d] = 1;
        char desc[320]; tl_signal_describe_disposition(d, desc, sizeof(desc));
        tl_log_line("signal-disposition: before a send of bionic %d on Darwin %d, the disposition reads back as: %s", gsig, d, desc);
    }
    if ((cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == host_signal_entry) return 0;   /* ours, already armed */
    int found = cur.sa_handler == SIG_DFL ? 0 : cur.sa_handler == SIG_IGN ? 1 : 2;
    int e = host_arm(d, gsig);
    tl_signal_note_rearm(gsig, d, e ? 1 : 0, e, found);
    return e ? -1 : 1;
}

static int b_sigaction(int sig, const guest_sigaction *act, guest_sigaction *old)
{
    if (sig <= 0 || sig >= 32) {
        /* Android's real-time signals (32..64) have no Darwin equivalent: the layer has no number to install the
         * guest's handler on or to deliver it with. Said once, in the layer's own words, because the guest's own
         * report of this ("setting blocked thread signal handler failed: Invalid argument" from libcore's
         * AsynchronousCloseMonitor) is the only other trace of it. */
        tl_note_once("sigaction refused a bionic signal >= 32: Darwin has no real-time signals, so the layer has no "
                     "number to install the guest's handler on -- Android's real-time signals are not delivered");
        tl_set_guest_errno(22);
        return -1;
    }
    int d = tl_signal_to_darwin(sig);
    if (d <= 0) { tl_set_guest_errno(22); return -1; }
    tl_signal_note_install(sig, d);
    if (old) {
        memset(old, 0, sizeof(*old));
        old->handler = g_guest_sig[sig].handler;
        old->sa_flags = g_guest_sig[sig].flags;
    }
    if (!act) return 0;
    g_guest_sig[sig].handler = act->handler;
    g_guest_sig[sig].flags = act->sa_flags;
    /* Synchronous fault signals stay with the host: the app's own crash handling and the
     * JIT guard depend on them, and a crash reporter replacing them would turn every
     * guest fault into silence. */
    if (is_fault_signal(sig)) {
        char note[96]; snprintf(note, sizeof(note), "guest installed a handler for fault signal %d: recorded, not installed", sig);
        tl_note_once(note);
        return 0;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    if (act->handler == (void *)0) sa.sa_handler = SIG_DFL;
    else if (act->handler == (void *)1) sa.sa_handler = SIG_IGN;
    else { sa.sa_sigaction = host_signal_entry; sa.sa_flags = SA_SIGINFO; }
    if (act->sa_flags & 0x10000000) sa.sa_flags |= SA_RESTART;
    sigemptyset(&sa.sa_mask);
    TL_ERRNO_BEGIN(); int r = sigaction(d, &sa, NULL); int hn = errno; TL_ERRNO_END();
    /* A host that REFUSED the install must not be recorded as "the handler is installed": the ledger would then claim a
     * delivery that can never run. Said once per signal, with the host's own errno. */
    if (r != 0) tl_signal_note_rearm(sig, d, 1, hn ? hn : EINVAL, 2);
    /* ROOT FIX -- the mask on the thread that just armed this handler. A signal can be blocked in a thread's mask without
     * the guest ever having blocked it: a thread the LAYER started that runs guest code is born with the HOST process's
     * mask by inheritance (tl_guest_mask_default clears it at the layer's own guest-entry points; this closes any thread
     * that does not call it -- an engine's main thread started by the layer, UnityMain among them). bionic's contract is
     * that a handler the thread does not itself block is delivered, so where the guest arms a REAL handler for `sig` on
     * such a thread, that Darwin number is brought in line with the guest's expectation. Otherwise every thread-directed
     * delivery of it stays PENDING on that thread and the handler never runs -- which is a collector's stop-the-world
     * handshake hanging forever. Guest threads are left ALONE: a thread a guest created has the guest's own mask, and this
     * acts only on a thread the LAYER started (tl_thread_is_guest()). It is the ARMING path -- a handful of calls per
     * process, never per frame, never per send -- and it touches no fault signal (those returned above) and nothing the
     * guest asked to ignore. */
    if (r == 0 && act->handler && act->handler != (void *)1 && !tl_thread_is_guest()) {
        sigset_t cur;
        if (pthread_sigmask(SIG_SETMASK, NULL, &cur) == 0) {
            uint32_t m; memcpy(&m, &cur, sizeof(m));
            if (m & (1u << (d - 1))) {
                sigset_t un; sigemptyset(&un); sigaddset(&un, d);
                errno = 0;
                int ur = pthread_sigmask(SIG_UNBLOCK, &un, NULL);
                tl_signal_note_mask_unblocked(sig, d, ur ? (errno ? errno : ur) : 0);
            }
        }
    }
    return r;
}
static void *b_signal(int sig, void *handler)
{
    guest_sigaction a = { .handler = handler }, o;
    if (b_sigaction(sig, &a, &o)) return (void *)-1;
    return o.handler;
}
static int b_sigemptyset(uint64_t *s) { *s = 0; return 0; }
static int b_sigfillset(uint64_t *s) { *s = ~0ull; return 0; }
static int b_sigaddset(uint64_t *s, int sig) { if (sig < 1 || sig > 64) { tl_set_guest_errno(22); return -1; } *s |= 1ull << (sig - 1); return 0; }
static int b_sigismember(const uint64_t *s, int sig) { if (sig < 1 || sig > 64) { tl_set_guest_errno(22); return -1; } return (int)((*s >> (sig - 1)) & 1); }
static int b_sigdelset(uint64_t *s, int sig) { if (sig < 1 || sig > 64) { tl_set_guest_errno(22); return -1; } *s &= ~(1ull << (sig - 1)); return 0; }
static int b_sigsuspend(const uint64_t *mask)
{
    uint32_t m = 0;
    for (int s = 1; s < 32; s++) if (*mask & (1ull << (s - 1))) { int d = tl_signal_to_darwin(s); if (d > 0) m |= 1u << (d - 1); }
    sigset_t ds; memcpy(&ds, &m, sizeof(m));
    sig_trace("sigsuspend begins, mask", (int)*mask);
    TL_ERRNO_BEGIN(); int r = sigsuspend(&ds); TL_ERRNO_END();
    sig_trace("sigsuspend returned", 0);
    return r;
}
typedef struct { void *ss_sp; int ss_flags; int pad; size_t ss_size; } guest_stack_t;
static int b_sigaltstack(const guest_stack_t *ss, guest_stack_t *old)
{
    stack_t d, od;
    /* Android's MINSIGSTKSZ is 5 KB, Darwin's 32 KB: crashpad's smaller stack is kept for the guest to read back, not installed */
    static __thread guest_stack_t small;
    if (ss && !(ss->ss_flags & 2) && ss->ss_size < MINSIGSTKSZ) {
        if (old) { *old = small; if (!old->ss_sp) old->ss_flags = 2; }
        small = *ss; return 0;
    }
    if (!ss && old && small.ss_sp) { *old = small; return 0; }
    if (ss) { d.ss_sp = ss->ss_sp; d.ss_size = ss->ss_size; d.ss_flags = ss->ss_flags & 2 ? SS_DISABLE : 0; }
    TL_ERRNO_BEGIN(); int r = sigaltstack(ss ? &d : NULL, old ? &od : NULL); TL_ERRNO_END();
    if (old && r == 0) { old->ss_sp = od.ss_sp; old->ss_size = od.ss_size; old->ss_flags = od.ss_flags & SS_DISABLE ? 2 : 0; }
    return r;
}
static void note_signal(const char *how, int sig)
{
    if (sig == 6 || sig == 9 || sig == 11 || sig == 15 || sig == 3) tl_log_line("bionic: guest asked for signal %d (%s)", sig, how);
}
static int b_raise(int sig) { note_signal("raise", sig); int d = tl_signal_to_darwin(sig); if (d < 0) { tl_signal_note_send(sig, pthread_self(), 1); tl_set_guest_errno(22); return -1; } tl_signal_arm_guest(d, sig); errno = 0; int r = raise(d); tl_signal_note_send_result(sig, pthread_self(), r ? (errno ? errno : r) : 0); return r; }


/* ------------------------------------------------- raw Linux system calls */

/*
 * What `svc #0` and syscall() mean here: the arm64 Linux numbers a guest actually uses,
 * answered with Darwin's equivalents. Returns the result, or -errno in Linux numbering.
 */
#define FUT_BUCKETS 256

/*
 * A futex wakes exactly the threads waiting on the address it is given. Waking a whole hash bucket
 * instead would wake waiters on unrelated addresses, and primitives built on futexes -- Unity's
 * semaphores and job queues among them -- are allowed to rely on that: a thread that returns from
 * FUTEX_WAIT has been woken for *its* word. So each waiter queues itself with its address and its
 * own condition variable, and a wake picks the matching ones, oldest first.
 */
typedef struct fut_waiter { uint32_t *addr; bool woken; pthread_cond_t cv; struct fut_waiter *next; } fut_waiter;
static struct { pthread_mutex_t m; fut_waiter *head, *tail; } g_fut[FUT_BUCKETS] = {
#define B { PTHREAD_MUTEX_INITIALIZER, NULL, NULL }
#define B8 B,B,B,B,B,B,B,B
#define B64 B8,B8,B8,B8,B8,B8,B8,B8
    B64, B64, B64, B64,
#undef B64
#undef B8
#undef B
};

static int g_futex_trace = -1;

static void fut_unlink(size_t h, fut_waiter *w)
{
    fut_waiter **pp = &g_fut[h].head, *prev = NULL;
    while (*pp && *pp != w) { prev = *pp; pp = &(*pp)->next; }
    if (!*pp) return;
    *pp = w->next;
    if (g_fut[h].tail == w) g_fut[h].tail = prev;
}

static long futex_call(uint32_t *addr, int op, uint32_t val, const struct timespec *to, uint32_t bitset_val)
{
    (void)bitset_val;
    int cmd = op & 127;
    if (g_futex_trace < 0) g_futex_trace = getenv("TL_FUTEX_TRACE") ? 1 : 0;
    size_t h = ((uintptr_t)addr >> 2) % FUT_BUCKETS;
    if (cmd == 1 || cmd == 10) {                                  /* FUTEX_WAKE / WAKE_BITSET */
        long woken = 0;
        pthread_mutex_lock(&g_fut[h].m);
        fut_waiter *w = g_fut[h].head, *next;
        for (; w && woken < (long)(int32_t)val; w = next) {
            next = w->next;
            if (w->addr != addr) continue;
            fut_unlink(h, w);
            w->woken = true;
            pthread_cond_signal(&w->cv);
            woken++;
        }
        pthread_mutex_unlock(&g_fut[h].m);
        if (g_futex_trace) { char nm[32] = ""; pthread_getname_np(pthread_self(), nm, sizeof(nm)); tl_log_line("futex[%s]: WAKE addr=%p n=%d woke %ld", nm, (void *)addr, (int)val, woken); }
        return woken;
    }
    if (cmd == 0 || cmd == 9) {                                   /* FUTEX_WAIT / WAIT_BITSET */
        pthread_mutex_lock(&g_fut[h].m);
        if (__atomic_load_n(addr, __ATOMIC_SEQ_CST) != val) { pthread_mutex_unlock(&g_fut[h].m); return -11; }
        fut_waiter me = { .addr = addr, .woken = false, .cv = PTHREAD_COND_INITIALIZER, .next = NULL };
        if (g_fut[h].tail) g_fut[h].tail->next = &me; else g_fut[h].head = &me;
        g_fut[h].tail = &me;
        if (g_futex_trace) { char nm[32] = ""; pthread_getname_np(pthread_self(), nm, sizeof(nm)); tl_log_line("futex[%s]: WAIT addr=%p val=%#x", nm, (void *)addr, val); }
        long r = 0;
        struct timespec rel = {0};
        if (to) {
            rel = *to;
            if (cmd == 9) {                                       /* absolute: CLOCK_MONOTONIC unless the realtime flag is set */
                struct timespec now;
                clock_gettime((op & 256) ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
                rel.tv_sec = to->tv_sec - now.tv_sec; rel.tv_nsec = to->tv_nsec - now.tv_nsec;
                if (rel.tv_nsec < 0) { rel.tv_sec--; rel.tv_nsec += 1000000000L; }
                if (rel.tv_sec < 0) { rel.tv_sec = 0; rel.tv_nsec = 0; }
            }
        }
        while (!me.woken) {
            if (!to) { pthread_cond_wait(&me.cv, &g_fut[h].m); continue; }
            if (pthread_cond_timedwait_relative_np(&me.cv, &g_fut[h].m, &rel) == ETIMEDOUT && !me.woken) { fut_unlink(h, &me); r = -110; break; }
            if (!me.woken) { rel.tv_sec = 0; rel.tv_nsec = 0; }   /* a spurious wake with a deadline: do not wait again past it */
        }
        pthread_mutex_unlock(&g_fut[h].m);
        pthread_cond_destroy(&me.cv);
        return r;
    }
    return -38;
}

static long linux_syscall_impl(long a0, long a1, long a2, long a3, long a4, long a5, long nr);
long tl_linux_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long nr)
{
    static int trace = -1;
    if (trace < 0) trace = getenv("TL_SYSCALL_TRACE") ? 1 : 0;
    long r = linux_syscall_impl(a0, a1, a2, a3, a4, a5, nr);
    if (trace) tl_log_line("syscall %ld(%#lx, %#lx, %#lx) -> %ld", nr, a0, a1, a2, r);
    return r;
}
static long linux_syscall_impl(long a0, long a1, long a2, long a3, long a4, long a5, long nr)
{
    (void)a4; (void)a5;
    switch (nr) {
    case 178: { uint64_t t = 0; pthread_threadid_np(NULL, &t); return (long)t; }       /* gettid */
    case 172: return getpid();
    case 173: return getppid();
    case 174: case 175: return getuid();
    case 176: case 177: return getgid();
    case 98:  return futex_call((uint32_t *)a0, (int)a1, (uint32_t)a2, (const struct timespec *)a3, (uint32_t)a5);
    case 129: case 130: case 131: {                                                     /* kill, tkill, tgkill */
        int sig = nr == 131 ? (int)a2 : (int)a1;
        if (sig == 0) return 0;
        note_signal("kill/tgkill system call", sig);
        int d = tl_signal_to_darwin(sig);
        if (d < 0) return -22;
        tl_signal_arm_guest(d, sig);
        /* tkill/tgkill name a THREAD. A bare raise() would run the guest's handler on the SENDER, so every guest that
         * signals another one through the raw syscall (Go's runtime, some protectors) would run it on the wrong thread.
         * The named tid is resolved in the layer's own live-thread census and the signal is sent to THAT thread; a tid
         * the layer cannot place is ESRCH, as the kernel answers, and never a raise() on the sender. */
        if (nr != 129) {
            uint64_t me = 0; pthread_threadid_np(NULL, &me);
            unsigned long tid = (unsigned long)(uint32_t)(nr == 131 ? a1 : a0);
            if (tid != (unsigned long)(uint32_t)me) {
                pthread_t target = tl_guest_pthread_for_tid(tid);
                if (!target) return -3;                                                 /* ESRCH: no such thread here */
                errno = 0;
                int dr = pthread_kill(target, d);
                tl_signal_note_send_result(sig, target, dr ? dr : (errno ? errno : 0));
                return dr == 0 ? 0 : -3;
            }
        }
        errno = 0;
        int rr = raise(d);
        tl_signal_note_send_result(sig, pthread_self(), rr ? (errno ? errno : rr) : 0);
        return rr == 0 ? 0 : -3;
    }
    case 56: {                                                                          /* openat: only AT_FDCWD */
        if ((int)a0 != -100) return -38;
        int fd = b_open((const char *)a1, (int)a2, (unsigned)a3);
        return fd < 0 ? -*tl_guest_errno_ptr() : fd;
    }
    case 57: { int r = b_close((int)a0); return r < 0 ? -(*tl_guest_errno_ptr() ? *tl_guest_errno_ptr() : 9 /* EBADF */) : r; }   /* through the shim: a raw close must drop the descriptor's redirect (see vfd_drop) */
    case 63: { if (vfd_is((int)a0)) { long r = b_read((int)a0, (void *)a1, (size_t)a2); return r < 0 ? -(*tl_guest_errno_ptr() ? *tl_guest_errno_ptr() : 5) : r; } long r = read((int)a0, kernel_dst((void *)a1, (size_t)a2), (size_t)a2); return r < 0 ? -tl_errno_to_guest(errno) : r; }
    case 64: { long r = write((int)a0, (const void *)a1, (size_t)a2); return r < 0 ? -tl_errno_to_guest(errno) : r; }
    case 113: { int r = clock_gettime(clock_to_darwin((int)a0), (struct timespec *)a1); return r < 0 ? -tl_errno_to_guest(errno) : 0; }
    case 169: return b_gettimeofday((int64_t *)a0, (void *)a1);
    case 93: case 94: tl_log_line("bionic: exit(%ld) by raw system call", a0); exit((int)a0);
    case 278: arc4random_buf((void *)a0, (size_t)a1); return a1;                        /* getrandom */
    case 283: return 0;                                                                 /* membarrier */
    case 134: {                                                                         /* rt_sigaction(sig, act, old, sigsetsize) */
        /* WAS a silent no-op ("accepted"). A guest that arms a handler through the raw syscall -- a runtime that avoids
         * libc, a packer, or a libc whose sigaction inlines the call -- got success and NO handler, and the layer never
         * saw an install at all, which reads exactly like a signal nobody ever armed. It is the same call the libc shim
         * makes, so it is made here too. */
        return b_sigaction((int)a0, (const guest_sigaction *)a1, (guest_sigaction *)a2) == 0 ? 0 : -22;
    }
    case 135: return tl_bionic_sigmask((int)a0, (const uint64_t *)a1, (uint64_t *)a2) == 0 ? 0 : -22;  /* rt_sigprocmask */
    case 233: return 0;                                                                 /* madvise */
    case 122: case 123: return 0;                                                       /* sched_setaffinity / getaffinity */
    case 167: return 0;                                                                 /* prctl */
    case 160: { int tl_bionic_uname(void *); return tl_bionic_uname((void *)a0); }    /* uname: filled in, as the libc call is */
    case 47: {                                                                          /* fallocate: growing the file (Superpack maps it at its full size, then writes) */
        if (a1) return -95;
        struct stat st;
        if (fstat((int)a0, &st)) return -tl_errno_to_guest(errno);
        if (st.st_size >= a2 + a3) return 0;
        return ftruncate((int)a0, a2 + a3) ? -tl_errno_to_guest(errno) : 0;
    }
    case 48: {                                                                          /* faccessat: only AT_FDCWD */
        if ((int)a0 != -100) return -38;
        char buf[1024]; const char *p = tl_path_resolve((const char *)a1, buf, sizeof(buf));
        return access(p, (int)a2) ? -tl_errno_to_guest(errno) : 0;
    }
    /* A guest that issues raw system calls instead of libc's (protectors and packers do) must get the same file as one that
     * calls libc: for a descriptor redirecting into an APK entry, read and lseek go through the same shims the wrapped
     * calls use, or the entry's descriptor would read the APK's own bytes at the APK's own offset. */
    case 62: { if (vfd_is((int)a0)) { long r = b_lseek((int)a0, (long)a1, (int)a2); return r < 0 ? -(*tl_guest_errno_ptr() ? *tl_guest_errno_ptr() : 22) : r; } off_t r = lseek((int)a0, (off_t)a1, (int)a2); return r < 0 ? -tl_errno_to_guest(errno) : (long)r; }
    case 101: { int r = nanosleep((const struct timespec *)a0, (struct timespec *)a1); return r < 0 ? -tl_errno_to_guest(errno) : 0; }
    case 124: sched_yield(); return 0;
    case 132: return 0;                                                                 /* sigaltstack: accepted (guest handlers are not installed) */
    /* memory and file status by raw system call (packers and protectors avoid libc): through the same shims as the calls */
    case 222: { void *r = b_mmap((void *)a0, (size_t)a1, (int)a2, (int)a3, (int)a4, a5); return r == (void *)-1 ? -*tl_guest_errno_ptr() : (long)r; }
    case 215: { int r = b_munmap((void *)a0, (size_t)a1); return r < 0 ? -*tl_guest_errno_ptr() : 0; }
    case 226: { int r = b_mprotect((void *)a0, (size_t)a1, (int)a2); return r < 0 ? -*tl_guest_errno_ptr() : 0; }
    case 79:  { int r = b_fstatat((int)a0, (const char *)a1, (guest_stat *)a2, (int)a3); return r < 0 ? -*tl_guest_errno_ptr() : 0; }
    case 80:  { int r = b_fstat((int)a0, (guest_stat *)a1); return r < 0 ? -*tl_guest_errno_ptr() : 0; }
    case 96: { uint64_t t = 0; pthread_threadid_np(NULL, &t); return (long)t; }        /* set_tid_address */
    case 99: return 0;                                                                  /* set_robust_list */
    case 117: return -1;                                                                /* ptrace: EPERM */
    default: break;
    }
    /* the rest, by the libc function bionic would have made the call through: called as the guest calls it (arguments in
       registers), -1 and errno turned into -errno as the kernel answers */
    {
        static const struct { short nr; const char *fn; } calls[] = {
            { 19, "eventfd" }, { 20, "epoll_create1" }, { 21, "epoll_ctl" }, { 22, "epoll_pwait" }, { 23, "dup" }, { 24, "dup3" },
            { 25, "fcntl" }, { 29, "ioctl" }, { 32, "flock" }, { 34, "mkdirat" }, { 35, "unlinkat" }, { 37, "linkat" }, { 38, "renameat" },
            { 43, "fstatfs" }, { 46, "ftruncate" }, { 49, "chdir" }, { 50, "fchdir" }, { 52, "fchmod" }, { 53, "fchmodat" }, { 55, "fchown" },
            { 59, "pipe2" }, { 61, "getdents64" }, { 65, "readv" }, { 66, "writev" }, { 67, "pread64" }, { 68, "pwrite64" }, { 69, "preadv" },
            { 71, "sendfile" }, { 73, "ppoll" }, { 78, "readlinkat" }, { 82, "fsync" }, { 83, "fdatasync" },
            { 85, "timerfd_create" }, { 86, "timerfd_settime" }, { 87, "timerfd_gettime" }, { 88, "utimensat" },
            { 114, "clock_getres" }, { 115, "clock_nanosleep" }, { 163, "getrlimit" }, { 164, "setrlimit" }, { 165, "getrusage" },
            { 166, "umask" }, { 179, "sysinfo" }, { 198, "socket" }, { 199, "socketpair" }, { 200, "bind" }, { 201, "listen" },
            { 202, "accept" }, { 203, "connect" }, { 204, "getsockname" }, { 205, "getpeername" }, { 206, "sendto" }, { 207, "recvfrom" },
            { 208, "setsockopt" }, { 209, "getsockopt" }, { 210, "shutdown" }, { 211, "sendmsg" }, { 212, "recvmsg" }, { 242, "accept4" },
            { 261, "prlimit64" }, { 281, "execveat" },
        };
        for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
            if (calls[i].nr != nr) continue;
            void *tl_bionic_find(const char *name);
            long (*f)(long, long, long, long, long, long) = (long (*)(long, long, long, long, long, long))tl_bionic_find(calls[i].fn);
            if (!f && nr == 261) f = (long (*)(long, long, long, long, long, long))tl_bionic_find("prlimit");
            if (!f) break;
            *tl_guest_errno_ptr() = 0;
            long r = f(a0, a1, a2, a3, a4, a5);
            if (nr == 166) return r;                                                    /* umask's answer is never an error */
            return r == -1 ? -(*tl_guest_errno_ptr() ? *tl_guest_errno_ptr() : 38) : r;
        }
    }
    char what[96];
    snprintf(what, sizeof(what), "raw system call %ld is not provided (ENOSYS)", nr);
    tl_note_once(what);
    return -38;
}

/* ------------------------------------------------------- poll, select, etc. */

/* Apple's poll() does not work on character devices: asked about /dev/urandom it answers POLLNVAL, where Linux says
 * "readable". OpenSSL seeds its RNG by polling that device, so it never read anything and refused to make a TLS connection
 * ("PRNG not seeded"). A descriptor poll() refuses is asked again through select(), which handles them. */
static int poll_devices_via_select(struct pollfd *fds, unsigned long n, int r)
{
    for (unsigned long i = 0; i < n; i++) {
        if (!(fds[i].revents & POLLNVAL) || fcntl(fds[i].fd, F_GETFD) < 0 || fds[i].fd >= FD_SETSIZE) continue;
        fd_set rs, ws; FD_ZERO(&rs); FD_ZERO(&ws);
        if (fds[i].events & POLLIN) FD_SET(fds[i].fd, &rs);
        if (fds[i].events & POLLOUT) FD_SET(fds[i].fd, &ws);
        struct timeval zero = { 0, 0 };
        int x = select(fds[i].fd + 1, &rs, &ws, NULL, &zero);
        short rev = 0;
        if (x > 0) { if (FD_ISSET(fds[i].fd, &rs)) rev |= POLLIN; if (FD_ISSET(fds[i].fd, &ws)) rev |= POLLOUT; }
        fds[i].revents = rev;
        if (!rev) r--;
    }
    return r;
}
static int b_poll(struct pollfd *fds, unsigned long n, int timeout)
{
    TL_ERRNO_BEGIN(); int r = poll(fds, (nfds_t)n, timeout); TL_ERRNO_END();
    if (r > 0) r = poll_devices_via_select(fds, n, r);
    for (unsigned long i = 0; i < n && i < 4; i++)
        if (net_trace_fd(fds[i].fd)) { tl_log_line("net: poll(%lu fds, fd[%lu] %d ev %#x, timeout %d) -> %d rev %#x", n, i, fds[i].fd, fds[i].events, timeout, r, r > 0 ? fds[i].revents : 0); break; }
    return r;
}
static int b_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
    TL_ERRNO_BEGIN(); int x = select(n, r, w, e, tv); int er = errno; TL_ERRNO_END();
    if (net_trace_fd(n - 1)) tl_log_line("net: select(%d, %s%s) -> %d", n, r ? "r" : "", w ? "w" : "", x);
    { static int tr = -1, said; if (tr < 0) tr = getenv("TL_FILE_TRACE") ? atoi(getenv("TL_FILE_TRACE")) : 0; if (tr && said++ < 40) tl_log_line("file: select(%d, %s%s%s, timeout %ldus) -> %d errno %d", n, r ? "r" : "", w ? "w" : "", e ? "e" : "", tv ? (long)tv->tv_sec * 1000000 + tv->tv_usec : -1L, x, x < 0 ? er : 0); }
    return x;
}
static void b___FD_SET_chk(int fd, uint64_t *set, size_t size)
{
    if (fd < 0 || (size_t)fd >= size * 8) { tl_log_line("bionic: __FD_SET_chk: fd %d out of range", fd); abort(); }
    set[fd / 64] |= 1ull << (fd % 64);
}
static int b___FD_ISSET_chk(int fd, const uint64_t *set, size_t size)
{
    if (fd < 0 || (size_t)fd >= size * 8) { tl_log_line("bionic: __FD_ISSET_chk: fd %d out of range", fd); abort(); }
    return (set[fd / 64] >> (fd % 64)) & 1;
}
static void *b___cmsg_nxthdr(void *msg, void *cmsg) { (void)msg; (void)cmsg; return NULL; }

/* Linux-only facilities. Failing cleanly is the honest answer until something needs more. */
static int stub_enosys_i(const char *what)
{
    char note[96]; snprintf(note, sizeof(note), "%s is not provided (ENOSYS)", what);
    tl_note_once(note);
    tl_set_guest_errno(38);
    return -1;
}
static int b_inotify_init(void) { return stub_enosys_i("inotify_init"); }
static int b_inotify_add_watch(int a, const char *b, unsigned c) { (void)a; (void)b; (void)c; return stub_enosys_i("inotify_add_watch"); }

const tl_bionic_entry tl_tab_io[] = {
    TL_WRAP("open", b_open), TL_WRAP("__open_2", b___open_2), TL_WRAP("close", b_close), TL_WRAP("read", b_read),
    TL_WRAP("__read_chk", b___read_chk), TL_WRAP("write", b_write), TL_WRAP("__write_chk", b___write_chk), TL_WRAP("writev", b_writev),
    TL_WRAP("pread64", b_pread64), TL_WRAP("pwrite64", b_pwrite64), TL_WRAP("pread", b_pread64), TL_WRAP("pwrite", b_pwrite64), TL_WRAP("__pread64_chk", b___pread64_chk), TL_WRAP("__pwrite64_chk", b___pwrite64_chk), TL_WRAP("__pwrite_chk", b___pwrite64_chk), TL_WRAP("__pread_chk", b___pread64_chk), TL_WRAP("lseek", b_lseek), TL_WRAP("lseek64", b_lseek),
    TL_WRAP("readv", b_readv), TL_WRAP("preadv", b_preadv),   /* the iovec forms of read must be served by the same redirect (see b_readv) */
    TL_WRAP("dup", b_dup), TL_WRAP("dup2", b_dup2), TL_WRAP("pipe", b_pipe), TL_WRAP("fsync", b_fsync), TL_WRAP("fdatasync", b_fsync),
    TL_WRAP("mkfifo", b_mkfifo), TL_WRAP("mknod", b_mknod), TL_WRAP("mknodat", b_mknodat), TL_WRAP("mkfifoat", b_mkfifoat),
    TL_WRAP("eventfd_write", b_eventfd_write), TL_WRAP("eventfd_read", b_eventfd_read), TL_WRAP("setmntent", b_setmntent),
    TL_WRAP("getmntent", b_getmntent), TL_WRAP("getmntent_r", b_getmntent_r), TL_WRAP("endmntent", b_endmntent),
    TL_WRAP("ftruncate", b_ftruncate), TL_WRAP("ftruncate64", b_ftruncate), TL_WRAP("truncate", b_truncate), TL_WRAP("isatty", b_isatty), TL_WRAP("flock", b_flock),
    TL_WRAP("unlink", b_unlink), TL_WRAP("rmdir", b_rmdir), TL_WRAP("mkdir", b_mkdir), TL_WRAP("access", b_access),
    TL_WRAP("chmod", b_chmod), TL_WRAP("fchmod", b_fchmod), TL_WRAP("link", b_link), TL_WRAP("symlink", b_symlink),
    TL_WRAP("readlink", b_readlink), TL_WRAP("realpath", b_realpath), TL_WRAP("getcwd", b_getcwd), TL_WRAP("__getcwd_chk", b___getcwd_chk),
    TL_WRAP("utimes", b_utimes), TL_WRAP("utime", b_utime), TL_WRAP("futimens", b_futimens),
    TL_WRAP("__umask_chk", b___umask_chk), TL_WRAP("sendfile", b_sendfile), TL_WRAP("sendfile64", b_sendfile),
    TL_WRAP("stat", b_stat), TL_WRAP("fstatat", b_fstatat), TL_WRAP("fstatat64", b_fstatat), TL_WRAP("lstat", b_lstat), TL_WRAP("fstat", b_fstat), TL_WRAP("lstat64", b_lstat), TL_WRAP("fstat64", b_fstat), TL_WRAP("statfs", b_statfs), TL_WRAP("statfs64", b_statfs), TL_WRAP("fstatfs", b_fstatfs), TL_WRAP("fstatfs64", b_fstatfs),
    TL_WRAP("fcntl", b_fcntl), TL_WRAP("ioctl", b_ioctl),
    TL_WRAP("scandir", b_scandir), TL_WRAP("alphasort", b_alphasort), TL_WRAP("versionsort", b_alphasort), TL_WRAP("opendir", b_opendir), TL_WRAP("fdopendir", b_fdopendir), TL_WRAP("dirfd", b_dirfd), TL_WRAP("rewinddir", b_rewinddir), TL_WRAP("readdir", b_readdir), TL_WRAP("closedir", b_closedir),
    TL_WRAP("mmap", b_mmap), TL_WRAP("mmap64", b_mmap), TL_WRAP("readdir64", b_readdir), TL_WRAP("munmap", b_munmap), TL_WRAP("mprotect", b_mprotect), TL_WRAP("madvise", b_madvise),
    TL_WRAP("mremap", b_mremap),
    TL_WRAP("clock_gettime", b_clock_gettime), TL_WRAP("clock_getres", b_clock_getres), TL_WRAP("gettimeofday", b_gettimeofday),
    TL_WRAP("nanosleep", b_nanosleep), TL_WRAP("clock_nanosleep", b_clock_nanosleep), TL_WRAP("usleep", b_usleep),
    TL_DIRECT(clock), TL_DIRECT(time), TL_DIRECT(difftime), TL_DIRECT(gmtime), TL_DIRECT(gmtime_r), TL_DIRECT(localtime),
    TL_DIRECT(localtime_r), TL_DIRECT(mktime), TL_DIRECT(strftime), TL_DIRECT(strftime_l), TL_DIRECT(tzset),
    TL_DIRECT(tfind), TL_DIRECT(tsearch), TL_DIRECT(tdelete), TL_DIRECT(twalk), TL_DIRECT(stpcpy),
    TL_DIRECT(ctime), TL_DIRECT(ctime_r), TL_DIRECT(asctime), TL_DIRECT(asctime_r), TL_DIRECT(timegm),
    TL_WRAP("sigaction", b_sigaction), TL_WRAP("signal", b_signal), TL_WRAP("sigemptyset", b_sigemptyset),
    TL_WRAP("sigfillset", b_sigfillset), TL_WRAP("sigaddset", b_sigaddset), TL_WRAP("sigdelset", b_sigdelset),
    /* LP64 bionic's sigset64_t is sigset_t, so the ...64 calls are the same ones (ART's signal chain asks for these) */
    TL_WRAP("sigaction64", b_sigaction), TL_WRAP("sigemptyset64", b_sigemptyset), TL_WRAP("sigfillset64", b_sigfillset),
    TL_WRAP("sigaddset64", b_sigaddset), TL_WRAP("sigdelset64", b_sigdelset),
    TL_WRAP("sigismember", b_sigismember), TL_WRAP("sigismember64", b_sigismember),
    TL_WRAP("sigsuspend", b_sigsuspend), TL_WRAP("sigaltstack", b_sigaltstack), TL_WRAP("raise", b_raise),
    TL_WRAP("poll", b_poll), TL_WRAP("select", b_select), TL_WRAP("__FD_SET_chk", b___FD_SET_chk),
    TL_WRAP("__FD_ISSET_chk", b___FD_ISSET_chk), TL_WRAP("__cmsg_nxthdr", b___cmsg_nxthdr),
    TL_WRAP("inotify_init", b_inotify_init), TL_WRAP("inotify_add_watch", b_inotify_add_watch),
    TL_END
};

/* ------------------------------------------------------- *at, stat64, statvfs */

/* The guest's AT_FDCWD is -100. With it (or an absolute path) the call is the plain one; with a real directory
 * descriptor Darwin resolves the path against it, which is what the guest meant. */
#define G_AT_FDCWD (-100)
#define G_AT_REMOVEDIR 0x200
#define G_AT_SYMLINK_NOFOLLOW 0x100
static bool at_plain(int dirfd, const char *p) { return dirfd == G_AT_FDCWD || (p && p[0] == '/'); }
static int b_openat(int dirfd, const char *path, int flags, unsigned mode);
/* FORTIFY's openat without a mode (no O_CREAT) */
static int b___openat_2(int dirfd, const char *path, int flags) { return b_openat(dirfd, path, flags, 0); }
static int b_openat(int dirfd, const char *path, int flags, unsigned mode)
{
    if (at_plain(dirfd, path)) return b_open(path, flags, mode);
    if ((flags & 3) == 0) fill_lib_standin_at(dirfd, path);
    TL_ERRNO_BEGIN(); int fd = openat(dirfd, path, oflags_to_darwin(flags), mode); int e = errno; TL_ERRNO_END();
    if (fd < 0 && e == ENOENT) tl_open_fail_note(path, e);
    return fd;
}
static int b_unlinkat(int dirfd, const char *path, int flags)
{
    if (at_plain(dirfd, path)) return (flags & G_AT_REMOVEDIR) ? b_rmdir(path) : b_unlink(path);
    TL_ERRNO_BEGIN(); int r = unlinkat(dirfd, path, (flags & G_AT_REMOVEDIR) ? AT_REMOVEDIR : 0); TL_ERRNO_END();
    return r;
}
static int b_fchmodat(int dirfd, const char *path, unsigned mode, int flags)
{
    (void)flags;
    if (at_plain(dirfd, path)) return b_chmod(path, mode);
    TL_ERRNO_BEGIN(); int r = fchmodat(dirfd, path, (mode_t)mode, 0); TL_ERRNO_END();
    return r;
}
/* The other *at calls, the same way: a path relative to AT_FDCWD (or absolute) goes through the guest's path mapping; one
 * relative to a real directory descriptor goes to the host's call. AT_EACCESS / AT_SYMLINK_NOFOLLOW are dropped where
 * Darwin's differ. */
static int b_faccessat(int dirfd, const char *path, int mode, int flags)
{
    (void)flags;
    if (at_plain(dirfd, path)) return b_access(path, mode);
    TL_ERRNO_BEGIN(); int r = faccessat(dirfd, path, mode, 0); TL_ERRNO_END();
    return r;
}
static int b_mkdirat(int dirfd, const char *path, unsigned mode)
{
    if (at_plain(dirfd, path)) return b_mkdir(path, mode);
    TL_ERRNO_BEGIN(); int r = mkdirat(dirfd, path, (mode_t)mode); TL_ERRNO_END();
    return r;
}
static long b_readlinkat(int dirfd, const char *path, char *buf, size_t n);
static long b___readlinkat_chk(int dirfd, const char *path, char *buf, size_t n, size_t bl) { (void)bl; return b_readlinkat(dirfd, path, buf, n); }
static long b_readlinkat(int dirfd, const char *path, char *buf, size_t n)
{
    if (at_plain(dirfd, path)) return b_readlink(path, buf, n);
    TL_ERRNO_BEGIN(); long r = readlinkat(dirfd, path, buf, n); TL_ERRNO_END();
    return r;
}
static long b___readlink_chk(const char *path, char *buf, size_t n, size_t bufsize)
{
    if (n > bufsize) { tl_log_line("bionic: __readlink_chk overflow"); abort(); }
    return b_readlink(path, buf, n);
}
static int b_renameat(int ofd, const char *op, int nfd, const char *np)
{
    if (at_plain(ofd, op) && at_plain(nfd, np)) {
        int (*ren)(const char *, const char *) = (int (*)(const char *, const char *))tl_bionic_find("rename");
        return ren(op, np);
    }
    char a[1024], b2[1024];
    const char *ho = at_plain(ofd, op) ? tl_path_resolve(op, a, sizeof(a)) : op, *hn = at_plain(nfd, np) ? tl_path_resolve(np, b2, sizeof(b2)) : np;
    TL_ERRNO_BEGIN(); int r = renameat(at_plain(ofd, op) ? AT_FDCWD : ofd, ho, at_plain(nfd, np) ? AT_FDCWD : nfd, hn); TL_ERRNO_END();
    return r;
}
static int b_symlinkat(const char *target, int dirfd, const char *path)
{
    if (at_plain(dirfd, path)) return b_symlink(target, path);
    TL_ERRNO_BEGIN(); int r = symlinkat(target, dirfd, path); TL_ERRNO_END();
    return r;
}
static int b___poll_chk(struct pollfd *fds, unsigned long n, int timeout, size_t fds_size)
{
    if (n > fds_size / sizeof(struct pollfd)) { tl_log_line("bionic: __poll_chk overflow"); abort(); }
    return b_poll(fds, n, timeout);
}
static int b_fchown(int fd, unsigned u, unsigned g) { TL_ERRNO_BEGIN(); int r = fchown(fd, u, g); TL_ERRNO_END(); return r; }
static int b_chdir(const char *p) { char b[1024]; TL_ERRNO_BEGIN(); int r = chdir(tl_path_resolve(p, b, sizeof(b))); TL_ERRNO_END(); return r; }
static int b_utimensat(int dirfd, const char *path, const struct timespec ts[2], int flags)
{
    struct timespec d[2];
    if (ts) for (int i = 0; i < 2; i++) {
        d[i] = ts[i];
        if (ts[i].tv_nsec == 0x3fffffff) d[i].tv_nsec = UTIME_NOW;            /* Linux UTIME_NOW  */
        else if (ts[i].tv_nsec == 0x3ffffffe) d[i].tv_nsec = UTIME_OMIT;      /* Linux UTIME_OMIT */
    }
    char b[1024];
    TL_ERRNO_BEGIN();
    int r = utimensat(at_plain(dirfd, path) ? AT_FDCWD : dirfd, at_plain(dirfd, path) ? tl_path_resolve(path, b, sizeof(b)) : path, ts ? d : NULL,
                      (flags & G_AT_SYMLINK_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : 0);
    TL_ERRNO_END();
    return r;
}
/* bionic's struct statvfs on LP64: the fields are all 8 bytes */
typedef struct { uint64_t f_bsize, f_frsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax; int32_t spare[6]; } guest_statvfs;
static int b_statvfs(const char *p, guest_statvfs *g)
{
    char b[1024]; struct statfs s;
    TL_ERRNO_BEGIN(); int r = statfs(tl_path_resolve(p, b, sizeof(b)), &s); TL_ERRNO_END();
    if (r == 0) {
        memset(g, 0, sizeof(*g));
        g->f_bsize = (uint64_t)s.f_bsize; g->f_frsize = (uint64_t)s.f_bsize; g->f_blocks = s.f_blocks; g->f_bfree = s.f_bfree; g->f_bavail = s.f_bavail;
        g->f_files = s.f_files; g->f_ffree = s.f_ffree; g->f_favail = s.f_ffree; g->f_namemax = 255;
    }
    return r;
}
static long b_pathconf(const char *p, int name)
{
    (void)p;
    switch (name) { case 3: return 255; case 4: return 4096; case 5: return 4096; case 6: return 0x10000; default: return -1; }
}

const tl_bionic_entry tl_tab_io2[] = {
    TL_WRAP("openat", b_openat), TL_WRAP("__openat_2", b___openat_2), TL_WRAP("faccessat", b_faccessat), TL_WRAP("mkdirat", b_mkdirat),
    TL_WRAP("readlinkat", b_readlinkat), TL_WRAP("__readlinkat_chk", b___readlinkat_chk), TL_WRAP("__readlink_chk", b___readlink_chk), TL_WRAP("renameat", b_renameat), TL_WRAP("symlinkat", b_symlinkat),
    TL_WRAP("__poll_chk", b___poll_chk), TL_WRAP("unlinkat", b_unlinkat), TL_WRAP("fchmodat", b_fchmodat), TL_WRAP("fchown", b_fchown),
    TL_WRAP("chdir", b_chdir), TL_WRAP("utimensat", b_utimensat), TL_WRAP("stat64", b_stat), TL_WRAP("statvfs", b_statvfs),
    TL_WRAP("statvfs64", b_statvfs), TL_WRAP("pathconf", b_pathconf),
    TL_END
};
