/* fs_glue.cpp — fs::File / fs::FS / FFat implementation over FatFs R0.15a
 * (fatfs/), volume 0 = the FFAT region inside the Gen 6 userdata partition.
 *
 * File handles come from a fixed pool (no heap churn) and are refcounted:
 * OWF passes File by value freely, and the FatFs object closes when the last
 * copy dies. Directory handles share the same pool (isdir discriminates).
 * FFat.begin() is the single storage entry point: eMMC -> GPT -> superblock
 * (storage_init) -> f_mount, f_mkfs(FAT32) on a virgin region.
 */
#include "FFat.h"
#include "SD_MMC.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../fatfs/ff.h"

extern "C" {
    int  storage_init(void);
    int  storage_ok(void);
    void con_puts(const char *s);
    void con_putdec(unsigned int v);
    void fb_trace(unsigned int xrgb);
    void timer_delay_ms(unsigned int ms);
}

extern "C" void storage_diag_set(unsigned int c);
#define STG_DIAG(c) storage_diag_set(c)   /* replayed post-setup, arduino_main */

FFatFS FFat;
SDMMCFS SD_MMC;

namespace fs {

struct FileImpl {
    int  refs = 0;
    bool isdir = false;
    bool opened = false;
    FIL  fil;
    DIR  dir;
    char fpath[192];
    char fname[64];
};

static FileImpl s_pool[8];
static FATFS    s_fatfs;
static bool     s_mounted;

static FileImpl *impl_alloc()
{
    for (auto &f : s_pool)
        if (f.refs == 0) { f = FileImpl(); f.refs = 1; return &f; }
    return nullptr;
}

static void impl_release(FileImpl *i)
{
    if (!i || --i->refs > 0) return;
    if (i->opened) {
        if (i->isdir) f_closedir(&i->dir);
        else          f_close(&i->fil);
        i->opened = false;
    }
}

static void split_name(FileImpl *i)
{
    const char *s = strrchr(i->fpath, '/');
    strncpy(i->fname, s ? s + 1 : i->fpath, sizeof(i->fname) - 1);
    i->fname[sizeof(i->fname) - 1] = 0;
}

/* ---- File ---------------------------------------------------------------- */
File::File(FileImpl *i) : _i(i) {}
File::File(const File &o) : _i(o._i) { if (_i) _i->refs++; }
File &File::operator=(const File &o)
{
    if (this == &o) return *this;
    FileImpl *old = _i;
    _i = o._i;
    if (_i) _i->refs++;
    impl_release(old);
    return *this;
}
File::~File() { impl_release(_i); _i = nullptr; }

File::operator bool() const { return _i && _i->opened; }

size_t File::read(uint8_t *buf, size_t n)
{
    if (!*this || _i->isdir) return 0;
    UINT got = 0;
    return f_read(&_i->fil, buf, (UINT)n, &got) == FR_OK ? got : 0;
}

int File::read()
{
    uint8_t c;
    return read(&c, 1) == 1 ? (int)c : -1;
}

size_t File::write(const uint8_t *buf, size_t n)
{
    if (!*this || _i->isdir) return 0;
    UINT put = 0;
    return f_write(&_i->fil, buf, (UINT)n, &put) == FR_OK ? put : 0;
}

size_t File::write(uint8_t c) { return write(&c, 1); }

int File::available()
{
    if (!*this || _i->isdir) return 0;
    FSIZE_t sz = f_size(&_i->fil), pos = f_tell(&_i->fil);
    return sz > pos ? (int)(sz - pos) : 0;
}

size_t File::size()     { return (*this && !_i->isdir) ? (size_t)f_size(&_i->fil) : 0; }
size_t File::position() { return (*this && !_i->isdir) ? (size_t)f_tell(&_i->fil) : 0; }
bool File::seek(uint32_t pos)
{ return *this && !_i->isdir && f_lseek(&_i->fil, pos) == FR_OK; }
void File::flush() { if (*this && !_i->isdir) f_sync(&_i->fil); }

void File::close()
{
    if (!_i) return;
    if (_i->opened) {
        if (_i->isdir) f_closedir(&_i->dir);
        else           f_close(&_i->fil);
        _i->opened = false;
    }
}

const char *File::name() { return _i ? _i->fname : ""; }
const char *File::path() { return _i ? _i->fpath : ""; }
bool File::isDirectory() { return _i && _i->isdir; }

File File::openNextFile()
{
    if (!*this || !_i->isdir) return File();
    FILINFO fi;
    if (f_readdir(&_i->dir, &fi) != FR_OK || fi.fname[0] == 0) return File();
    char child[256];
    snprintf(child, sizeof(child), "%s%s%s", _i->fpath,
             (_i->fpath[0] && _i->fpath[strlen(_i->fpath) - 1] == '/') ? "" : "/",
             fi.fname);

    FileImpl *ci = impl_alloc();
    if (!ci) return File();
    strncpy(ci->fpath, child, sizeof(ci->fpath) - 1);
    split_name(ci);
    if (fi.fattrib & AM_DIR) {
        ci->isdir = true;
        if (f_opendir(&ci->dir, child) != FR_OK) { ci->refs = 0; return File(); }
    } else {
        if (f_open(&ci->fil, child, FA_READ) != FR_OK) { ci->refs = 0; return File(); }
    }
    ci->opened = true;
    return File(ci);
}

String File::readString()
{
    String out;
    uint8_t buf[64];
    size_t n;
    while ((n = read(buf, sizeof(buf))) > 0)
        for (size_t i = 0; i < n; i++) out += (char)buf[i];
    return out;
}

String File::readStringUntil(char term)
{
    String out;
    int c;
    while ((c = read()) >= 0 && (char)c != term) out += (char)c;
    return out;
}

size_t File::readBytesUntil(char term, char *buf, size_t n)
{
    size_t got = 0;
    int c;
    while (got < n && (c = read()) >= 0 && (char)c != term)
        buf[got++] = (char)c;
    return got;
}

int File::print(const char *s)
{ return s ? (int)write((const uint8_t *)s, strlen(s)) : 0; }

int File::printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    size_t take = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    return (int)write((const uint8_t *)buf, take);
}

/* ---- FS ------------------------------------------------------------------ */
static BYTE mode_flags(const char *mode)
{
    if (!mode) return FA_READ;
    bool w = false, a = false, plus = false;
    for (const char *p = mode; *p; p++) {
        if (*p == 'w') w = true;
        if (*p == 'a') a = true;
        if (*p == '+') plus = true;
    }
    if (a) return (BYTE)(FA_WRITE | FA_OPEN_APPEND | (plus ? FA_READ : 0));
    if (w) return (BYTE)(FA_WRITE | FA_CREATE_ALWAYS | (plus ? FA_READ : 0));
    return (BYTE)(FA_READ | (plus ? FA_WRITE : 0));
}

File FS::open(const char *path, const char *mode)
{
    if (!s_mounted || !path || !path[0]) return File();

    FileImpl *i = impl_alloc();
    if (!i) return File();
    strncpy(i->fpath, path, sizeof(i->fpath) - 1);
    split_name(i);

    FILINFO fi;
    bool isdir = (strcmp(path, "/") == 0);
    if (!isdir && f_stat(path, &fi) == FR_OK && (fi.fattrib & AM_DIR)) isdir = true;

    if (isdir) {
        i->isdir = true;
        if (f_opendir(&i->dir, path) != FR_OK) { i->refs = 0; return File(); }
    } else {
        if (f_open(&i->fil, path, mode_flags(mode)) != FR_OK) { i->refs = 0; return File(); }
    }
    i->opened = true;
    return File(i);
}

bool FS::exists(const char *path)
{
    if (!s_mounted || !path) return false;
    if (strcmp(path, "/") == 0) return true;
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK;
}

bool FS::remove(const char *path) { return s_mounted && f_unlink(path) == FR_OK; }
bool FS::rename(const char *f, const char *t) { return s_mounted && f_rename(f, t) == FR_OK; }
bool FS::mkdir(const char *path)  { return s_mounted && f_mkdir(path) == FR_OK; }
bool FS::rmdir(const char *path)  { return s_mounted && f_unlink(path) == FR_OK; }

static size_t vol_bytes(bool used)
{
    if (!s_mounted) return 0;
    FATFS *pfs;
    DWORD nfree;
    if (f_getfree("", &nfree, &pfs) != FR_OK) return 0;
    size_t total = (size_t)(pfs->n_fatent - 2) * pfs->csize * 512u;
    size_t freeb = (size_t)nfree * pfs->csize * 512u;
    return used ? total - freeb : total;
}

size_t FS::totalBytes() { return vol_bytes(false); }
size_t FS::usedBytes()  { return vol_bytes(true); }

} // namespace fs

/* ---- FFat ---------------------------------------------------------------- */
bool FFatFS::begin(bool format_on_fail, const char *, uint8_t, const char *)
{
    if (fs::s_mounted) return true;
    if (!storage_ok() && storage_init() < 0) return false;

    FRESULT fr = f_mount(&fs::s_fatfs, "", 1);
    if (fr == FR_NO_FILESYSTEM || (fr != FR_OK && format_on_fail)) {
        con_puts("ffat: no filesystem - formatting (FAT32)\n");
        static BYTE work[16384];
        MKFS_PARM parm = { FM_FAT32, 0, 0, 0, 0 };
        fr = f_mkfs("", &parm, work, sizeof(work));
        if (fr != FR_OK) {
            con_puts("ffat: mkfs failed "); con_putdec((unsigned)fr); con_puts("\n");
            return false;
        }
        fr = f_mount(&fs::s_fatfs, "", 1);
    }
    if (fr != FR_OK) {
        con_puts("ffat: mount failed "); con_putdec((unsigned)fr); con_puts("\n");
        STG_DIAG(0x0000FFu);        /* blue: storage up, filesystem failed */
        return false;
    }
    fs::s_mounted = true;
    con_puts("ffat: mounted\n");
    STG_DIAG(0x00FF00u);            /* green: whole chain verified */
    return true;
}

void FFatFS::end()
{
    if (!fs::s_mounted) return;
    f_unmount("");
    fs::s_mounted = false;
}

bool FFatFS::format()
{
    static BYTE work[16384];
    MKFS_PARM parm = { FM_FAT32, 0, 0, 0, 0 };
    if (fs::s_mounted) { f_unmount(""); fs::s_mounted = false; }
    if (!storage_ok() && storage_init() < 0) return false;
    if (f_mkfs("", &parm, work, sizeof(work)) != FR_OK) return false;
    if (f_mount(&fs::s_fatfs, "", 1) != FR_OK) return false;
    fs::s_mounted = true;
    return true;
}

size_t FFatFS::totalBytes() { return fs::FS::totalBytes(); }
size_t FFatFS::usedBytes()  { return fs::FS::usedBytes(); }
size_t FFatFS::freeBytes()  { return totalBytes() - usedBytes(); }
