/* Rekarina as one file: Rekarina.exe carries the whole setup folder (tools/make_rekarina_release.py --single) and
   starts its launcher, unpacking the folder first where that has not been done.

   The file is this program, then a zip of the setup folder, then a trailer of 56 bytes: "RKSFX001", the zip's offset
   and size (two 64-bit little-endian numbers) and the version (32 bytes, zero padded). The folder goes to
   <Rekarina's folder>\setup\<version>, the folder being the player's choice at the first start (find_home); a file
   named unpacked.txt, written last, says it is whole and which way it was unpacked. This program unpacks the zip
   itself (unzip_builtin: stored and deflated files, every file's CRC-32 checked, paths of any length, on a few
   threads: the 1.2.8 setup in 2 s); where that fails, the zip is copied out of this file and Windows' own tar.exe
   (bsdtar, part of Windows 10 since 1803) has a go, which cannot enter a folder whose path has letters outside the
   system's code page and took 6 s. All of it in a thread, while a small window says what is going on; each attempt
   is a line in <folder>\unpack.log. Then <folder>\Rekarina-Launcher.exe starts with this program's arguments
   (--unpack-only: unpack, start nothing, show nothing: a failure is a line in unpack.log and exit code 1). Updates
   are the launcher's own business from there (installer/update.py): they change that folder.
   For tests/single_unpack_test.py: REKARINA_UNPACK_BUILTIN=off skips the built-in way, REKARINA_UNPACK_TAR=<path>
   names the tar.exe to fall back on, "off" none.

   Messages are in Russian on a Russian Windows and in English elsewhere, as the launcher exe's are.

   Build: llvm/bin/llvm-windres -I runtime -O coff installer/launcher/rekarina.rc -o rekarina_rc.o, then
   llvm/bin/clang -O2 -mwindows -municode installer/launcher/rekarina_single.c rekarina_rc.o -lole32 -luuid
   -lshell32 -o Rekarina.exe */
#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>
#include <wchar.h>

#define PATH_CHARS (MAX_PATH * 2)
#define TRAILER 56

static wchar_t g_self[PATH_CHARS], g_dir[PATH_CHARS], g_zip[PATH_CHARS], g_version[40];
static unsigned long long g_offset, g_size;
static HWND g_window;
static volatile LONG g_result = -1;   /* the unpacking thread's answer: 0 done, 1 a file could not be written, 2 unpacking failed */
static wchar_t g_why[1024];          /* what went wrong, for the message and unpack.log */
static int g_quiet;                  /* --unpack-only: no window, no message box */

static int russian(void) { return (GetUserDefaultUILanguage() & 0x3ff) == 0x19; }

static int exists(const wchar_t *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

/* A line of <folder>\unpack.log (UTF-8), with the time and the version. */
static void log_line(const wchar_t *text) {
    if (!g_dir[0]) return;
    wchar_t path[PATH_CHARS + 16], line[4096];
    swprintf(path, PATH_CHARS + 16, L"%ls\\unpack.log", g_dir);
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf(line, 4096, L"%04u-%02u-%02u %02u:%02u:%02u %ls: %ls\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond, g_version, text);
    for (wchar_t *c = line; c[0] && c[1] && c[2]; c++)   /* one line: a message's own line breaks become spaces */
        if (*c == L'\n' || *c == L'\r') *c = L' ';
    static char utf8[12288];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof utf8, NULL, NULL);
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put;
    if (n > 1) WriteFile(f, utf8, (DWORD)(n - 1), &put, NULL);
    CloseHandle(f);
}

static void fail(const wchar_t *en, const wchar_t *ru) {
    if (g_quiet) log_line(en);   /* a test or a script: nothing on the screen */
    else MessageBoxW(NULL, russian() ? ru : en, L"Rekarina", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

/* The trailer of this file: the zip's place and the version. 0 when the file carries none. */
static int read_trailer(wchar_t *version, int chars) {
    HANDLE f = CreateFileW(g_self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size, at;
    unsigned char t[TRAILER];
    DWORD got = 0;
    int ok = GetFileSizeEx(f, &size) && size.QuadPart > TRAILER;
    at.QuadPart = size.QuadPart - TRAILER;
    ok = ok && SetFilePointerEx(f, at, NULL, FILE_BEGIN) && ReadFile(f, t, TRAILER, &got, NULL) && got == TRAILER &&
         !memcmp(t, "RKSFX001", 8);
    CloseHandle(f);
    if (!ok) return 0;
    memcpy(&g_offset, t + 8, 8);
    memcpy(&g_size, t + 16, 8);
    if (g_offset + g_size + TRAILER != (unsigned long long)size.QuadPart) return 0;
    int n = 0;
    for (; n < 31 && n < chars - 1 && t[24 + n]; n++) {
        unsigned char c = t[24 + n];   /* a version is digits, letters, dots and dashes: nothing a path cannot hold */
        version[n] = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' ? c : L'_';
    }
    version[n] = 0;
    return n > 0;
}

static int make_dirs(wchar_t *path) {   /* every folder of path, which ends without a slash */
    for (wchar_t *p = path + 3; *p; p++)
        if (*p == L'\\') {
            *p = 0;
            CreateDirectoryW(path, NULL);
            *p = L'\\';
        }
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int run(wchar_t *command, const wchar_t *cwd, int wait, DWORD flags) {
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, flags, NULL, cwd, &si, &pi)) return -1;
    DWORD code = 0;
    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

/* ---- The built-in unpacker: inflate (RFC 1951) and the zip's directory, for a Windows without a working tar.exe */

static unsigned g_crc_table[256];

static void crc_init(void) {
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[i] = c;
    }
}

static unsigned crc32_of(const unsigned char *p, size_t n) {
    unsigned c = 0xFFFFFFFFu;
    while (n--) c = g_crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

typedef struct {
    const unsigned char *in;
    size_t inlen, pos;
    unsigned long long bb;   /* bits not used yet, the next one lowest */
    int bc;                  /* how many */
    unsigned char *out;
    size_t outlen, outpos;
} Inflate;

static void inf_fill(Inflate *s) {
    while (s->bc <= 56 && s->pos < s->inlen) {
        s->bb |= (unsigned long long)s->in[s->pos++] << s->bc;
        s->bc += 8;
    }
}

static int inf_bits(Inflate *s, int n, unsigned *v) {   /* n of 0..16 bits; 0 when the data ends first */
    if (s->bc < n) inf_fill(s);
    if (s->bc < n) return 0;
    *v = (unsigned)(s->bb & ((1ull << n) - 1));
    s->bb >>= n;
    s->bc -= n;
    return 1;
}

/* A Huffman code: canonical, by its code lengths. Codes of up to FAST_BITS bits are found in one look-up of the next
   bits (as they come off the stream: the code's first bit lowest), longer ones bit by bit as in zlib's puff.c. */
#define FAST_BITS 10
typedef struct {
    unsigned short fast[1 << FAST_BITS];   /* length << 9 | symbol; 0: longer, or no such code */
    unsigned short count[16];              /* codes of each length */
    unsigned short symbol[288];            /* the symbols in code order */
} Huffman;

static int huff_build(Huffman *h, const unsigned char *length, int n) {
    unsigned short offs[16];
    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left = (left << 1) - h->count[l];
        if (left < 0) return 0;   /* more codes than the lengths can hold */
    }
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++)
        if (length[i]) h->symbol[offs[length[i]]++] = (unsigned short)i;
    unsigned code = 0;
    int k = 0;
    for (int l = 1; l < 16; l++, code <<= 1)
        for (int c = 0; c < h->count[l]; c++, k++, code++)
            if (l <= FAST_BITS) {
                unsigned r = 0;
                for (int b = 0; b < l; b++) r |= ((code >> b) & 1u) << (l - 1 - b);
                for (unsigned j = r; j < (1u << FAST_BITS); j += 1u << l) h->fast[j] = (unsigned short)(l << 9 | h->symbol[k]);
            }
    return 1;
}

static int huff_decode(Inflate *s, const Huffman *h) {   /* the next symbol, or -1 */
    if (s->bc < 15) inf_fill(s);
    unsigned e = h->fast[s->bb & ((1u << FAST_BITS) - 1)];
    if (e) {
        int l = (int)(e >> 9);
        if (l > s->bc) return -1;
        s->bb >>= l;
        s->bc -= l;
        return (int)(e & 511);
    }
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        if (l > s->bc) return -1;
        code |= (int)((s->bb >> (l - 1)) & 1);
        int count = h->count[l];
        if (code - count < first) {
            s->bb >>= l;
            s->bc -= l;
            return h->symbol[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static const unsigned short LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83,
                                            99, 115, 131, 163, 195, 227, 258};
static const unsigned char LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const unsigned short DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                             1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const unsigned char DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
                                             12, 12, 13, 13};
static Huffman g_fixed_lit, g_fixed_dist;

static void fixed_init(void) {
    unsigned char l[288];
    for (int i = 0; i < 288; i++) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    huff_build(&g_fixed_lit, l, 288);
    for (int i = 0; i < 30; i++) l[i] = 5;
    huff_build(&g_fixed_dist, l, 30);
}

static int inf_stored(Inflate *s) {
    s->bb >>= s->bc & 7;   /* to the byte boundary; the whole bytes still held go back to the input */
    s->bc -= s->bc & 7;
    s->pos -= (size_t)(s->bc / 8);
    s->bb = 0, s->bc = 0;
    if (s->inlen - s->pos < 4) return 0;
    const unsigned char *p = s->in + s->pos;
    unsigned len = p[0] | p[1] << 8, nlen = p[2] | p[3] << 8;
    s->pos += 4;
    if (len != (~nlen & 0xFFFFu) || s->inlen - s->pos < len || s->outlen - s->outpos < len) return 0;
    memcpy(s->out + s->outpos, s->in + s->pos, len);
    s->pos += len;
    s->outpos += len;
    return 1;
}

static int inf_codes(Inflate *s, const Huffman *lit, const Huffman *dist) {
    for (;;) {
        int sym = huff_decode(s, lit);
        if (sym < 0) return 0;
        if (sym < 256) {
            if (s->outpos >= s->outlen) return 0;
            s->out[s->outpos++] = (unsigned char)sym;
            continue;
        }
        if (sym == 256) return 1;
        sym -= 257;
        unsigned v;
        if (sym >= 29 || !inf_bits(s, LEN_EXTRA[sym], &v)) return 0;
        size_t len = LEN_BASE[sym] + v;
        int ds = huff_decode(s, dist);
        if (ds < 0 || ds >= 30 || !inf_bits(s, DIST_EXTRA[ds], &v)) return 0;
        size_t d = DIST_BASE[ds] + v;
        if (d > s->outpos || len > s->outlen - s->outpos) return 0;
        unsigned char *o = s->out + s->outpos;
        const unsigned char *from = o - d;
        for (size_t i = 0; i < len; i++) o[i] = from[i];   /* a byte at a time: the copy may overlap what it writes */
        s->outpos += len;
    }
}

static int inf_dynamic(Inflate *s, Huffman *lit, Huffman *dist) {
    static const unsigned char ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    unsigned nlen, ndist, ncode, v;
    if (!inf_bits(s, 5, &nlen) || !inf_bits(s, 5, &ndist) || !inf_bits(s, 4, &ncode)) return 0;
    nlen += 257, ndist += 1, ncode += 4;
    if (nlen > 286 || ndist > 30) return 0;
    unsigned char lengths[320];
    memset(lengths, 0, sizeof lengths);
    for (unsigned i = 0; i < ncode; i++) {
        if (!inf_bits(s, 3, &v)) return 0;
        lengths[ORDER[i]] = (unsigned char)v;
    }
    if (!huff_build(lit, lengths, 19)) return 0;   /* (lit holds the code lengths' code for now) */
    memset(lengths, 0, sizeof lengths);
    for (unsigned i = 0; i < nlen + ndist;) {
        int sym = huff_decode(s, lit);
        if (sym < 0) return 0;
        if (sym < 16) {
            lengths[i++] = (unsigned char)sym;
            continue;
        }
        unsigned char fill = 0;
        unsigned rep;
        if (sym == 16) {
            if (!i || !inf_bits(s, 2, &v)) return 0;
            fill = lengths[i - 1], rep = 3 + v;
        } else if (sym == 17) {
            if (!inf_bits(s, 3, &v)) return 0;
            rep = 3 + v;
        } else {
            if (!inf_bits(s, 7, &v)) return 0;
            rep = 11 + v;
        }
        if (i + rep > nlen + ndist) return 0;
        while (rep--) lengths[i++] = fill;
    }
    if (!lengths[256]) return 0;   /* no end of block */
    return huff_build(lit, lengths, (int)nlen) && huff_build(dist, lengths + nlen, (int)ndist);
}

/* Inflates in into out, which must come out exactly full. 1 when it did. */
static int inflate_all(const unsigned char *in, size_t inlen, unsigned char *out, size_t outlen) {
    Inflate s = {.in = in, .inlen = inlen, .out = out, .outlen = outlen};
    Huffman *t = HeapAlloc(GetProcessHeap(), 0, 2 * sizeof *t);
    if (!t) return 0;
    unsigned last = 0, type;
    int ok = 1;
    while (ok && !last) {
        if (!inf_bits(&s, 1, &last) || !inf_bits(&s, 2, &type)) ok = 0;
        else if (type == 0) ok = inf_stored(&s);
        else if (type == 1) ok = inf_codes(&s, &g_fixed_lit, &g_fixed_dist);
        else if (type == 2) ok = inf_dynamic(&s, &t[0], &t[1]) && inf_codes(&s, &t[0], &t[1]);
        else ok = 0;
    }
    HeapFree(GetProcessHeap(), 0, t);
    return ok && s.outpos == outlen;
}

/* The zip's directory, read from this file (offsets in it count from the zip's start, g_offset). */
typedef struct {
    unsigned long long csize, usize, local;
    unsigned crc;
    unsigned short method, flags, date, time;
    wchar_t *name;   /* relative, with backslashes */
} Entry;

static Entry *g_entries;
static LONG g_nentries, g_next;
static volatile LONG g_stop;   /* a worker failed: the others take no more */
static LONG g_fail_kind;       /* 1 writing, 2 the zip */
static CRITICAL_SECTION g_why_lock;
static wchar_t g_full[PATH_CHARS + 8];   /* \\?\ and g_dir, so no path is held to 260 characters */

static void builtin_fail(LONG kind, const wchar_t *what, const wchar_t *name) {
    EnterCriticalSection(&g_why_lock);
    if (!g_stop) {
        g_fail_kind = kind;
        size_t n = 0;   /* (copied in pieces: a member's name may be longer than the buffer) */
        if (name) {
            wcsncpy(g_why, name, 800);
            g_why[800] = 0;
            n = wcslen(g_why);
            wcscpy(g_why + n, L": ");
            n += 2;
        }
        wcsncpy(g_why + n, what, 1023 - n);
        g_why[1023] = 0;
        InterlockedExchange(&g_stop, 1);
    }
    LeaveCriticalSection(&g_why_lock);
}

static int read_at(HANDLE f, unsigned long long at, void *buf, size_t n) {
    unsigned char *p = buf;
    while (n) {
        OVERLAPPED o = {.Offset = (DWORD)at, .OffsetHigh = (DWORD)(at >> 32)};
        DWORD want = n > (1u << 30) ? 1u << 30 : (DWORD)n, got = 0;
        if (!ReadFile(f, p, want, &got, &o) || got != want) return 0;
        p += got, at += got, n -= got;
    }
    return 1;
}

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static unsigned long long rd64(const unsigned char *p) { return rd32(p) | (unsigned long long)rd32(p + 4) << 32; }

/* A member's name as a path under the folder: UTF-8 to UTF-16, slashes turned, nothing that leaves the folder. */
static wchar_t *entry_name(const unsigned char *p, unsigned n) {
    if (!n) return NULL;
    wchar_t *w = HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(wchar_t));
    int m = w ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)p, (int)n, w, (int)n) : 0;
    if (m <= 0) return NULL;
    w[m] = 0;
    for (int i = 0; i < m; i++) {
        if (w[i] == L'/') w[i] = L'\\';
        if (w[i] == L':' || w[i] < 32) return NULL;
    }
    if (w[0] == L'\\') return NULL;
    for (wchar_t *part = w; *part;) {   /* no ".." part */
        wchar_t *end = wcschr(part, L'\\');
        size_t len = end ? (size_t)(end - part) : wcslen(part);
        if (len == 2 && part[0] == L'.' && part[1] == L'.') return NULL;
        if (!end) break;
        part = end + 1;
    }
    return w;
}

static int read_directory(HANDLE f) {   /* g_entries from the zip's central directory; 0 when it is not a zip we read */
    unsigned char tail[22 + 20], fixed[56];
    if (g_size < 22 || !read_at(f, g_offset + g_size - 22, tail + 20, 22) || rd32(tail + 20) != 0x06054b50) return 0;
    unsigned long long n = rd16(tail + 20 + 10), cd_size = rd32(tail + 20 + 12), cd_at = rd32(tail + 20 + 16);
    if (n == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_at == 0xFFFFFFFFu) {   /* zip64: its locator, then its end record */
        if (g_size < 42 || !read_at(f, g_offset + g_size - 42, tail, 20) || rd32(tail) != 0x07064b50) return 0;
        unsigned long long end64 = rd64(tail + 8);
        if (end64 + 56 > g_size || !read_at(f, g_offset + end64, fixed, 56) || rd32(fixed) != 0x06064b50) return 0;
        n = rd64(fixed + 32), cd_size = rd64(fixed + 40), cd_at = rd64(fixed + 48);
    }
    if (cd_at + cd_size > g_size || n > cd_size / 46 || cd_size > (1ull << 31)) return 0;
    unsigned char *cd = HeapAlloc(GetProcessHeap(), 0, (size_t)cd_size + 1);
    g_entries = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)(n + 1) * sizeof *g_entries);
    if (!cd || !g_entries || !read_at(f, g_offset + cd_at, cd, (size_t)cd_size)) return 0;
    size_t at = 0;
    for (unsigned long long i = 0; i < n; i++) {
        const unsigned char *p = cd + at;
        if (cd_size - at < 46 || rd32(p) != 0x02014b50) return 0;
        unsigned nlen = rd16(p + 28), elen = rd16(p + 30), clen = rd16(p + 32);
        if (cd_size - at < 46ull + nlen + elen + clen) return 0;
        Entry *e = &g_entries[i];
        e->flags = (unsigned short)rd16(p + 8), e->method = (unsigned short)rd16(p + 10);
        e->time = (unsigned short)rd16(p + 12), e->date = (unsigned short)rd16(p + 14);
        e->crc = rd32(p + 16), e->csize = rd32(p + 20), e->usize = rd32(p + 24), e->local = rd32(p + 42);
        for (const unsigned char *x = p + 46 + nlen, *xe = x + elen; xe - x >= 4;) {   /* the zip64 sizes and offset */
            unsigned id = rd16(x), len = rd16(x + 2);
            const unsigned char *v = x + 4;
            if ((size_t)(xe - v) < len) break;
            if (id == 1) {
                const unsigned char *ve = v + len;
                if (e->usize == 0xFFFFFFFFu && ve - v >= 8) e->usize = rd64(v), v += 8;
                if (e->csize == 0xFFFFFFFFu && ve - v >= 8) e->csize = rd64(v), v += 8;
                if (e->local == 0xFFFFFFFFu && ve - v >= 8) e->local = rd64(v), v += 8;
            }
            x = x + 4 + len;
        }
        if (!(e->name = entry_name(p + 46, nlen))) return 0;
        at += 46 + nlen + elen + clen;
    }
    g_nentries = (LONG)n;
    HeapFree(GetProcessHeap(), 0, cd);
    return 1;
}

static int make_dirs_long(wchar_t *path, size_t from) {   /* every folder of a \\?\ path from its character `from` on */
    for (wchar_t *p = path + from; *p; p++)
        if (*p == L'\\') {
            *p = 0;
            CreateDirectoryW(path, NULL);
            *p = L'\\';
        }
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int write_entry(const Entry *e, const unsigned char *data, size_t n) {
    size_t room = wcslen(g_full) + wcslen(e->name) + 2;
    wchar_t *path = HeapAlloc(GetProcessHeap(), 0, room * sizeof(wchar_t));
    if (!path) return 0;
    swprintf(path, room, L"%ls\\%ls", g_full, e->name);
    size_t base = wcslen(g_full) + 1;
    size_t len = wcslen(path);
    int ok;
    if (len && path[len - 1] == L'\\') {   /* a folder */
        path[len - 1] = 0;
        ok = make_dirs_long(path, base);
    } else {
        HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND) {
            wchar_t *cut = wcsrchr(path, L'\\');
            *cut = 0;
            make_dirs_long(path, base);
            *cut = L'\\';
            h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
        ok = h != INVALID_HANDLE_VALUE;
        while (ok && n) {
            DWORD want = n > (1u << 30) ? 1u << 30 : (DWORD)n, put = 0;
            ok = WriteFile(h, data, want, &put, NULL) && put == want;
            data += want, n -= want;
        }
        FILETIME local, utc;   /* the zip's time, local as tar takes it */
        if (ok && DosDateTimeToFileTime(e->date, e->time, &local) && LocalFileTimeToFileTime(&local, &utc))
            SetFileTime(h, NULL, NULL, &utc);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    HeapFree(GetProcessHeap(), 0, path);
    return ok;
}

static DWORD WINAPI unzip_worker(void *arg) {
    (void)arg;
    HANDLE f = CreateFileW(g_self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        builtin_fail(2, L"this program could not read itself", NULL);
        return 0;
    }
    while (!g_stop) {
        LONG i = InterlockedIncrement(&g_next) - 1;
        if (i >= g_nentries) break;
        const Entry *e = &g_entries[i];
        unsigned char local[30];
        if (e->flags & 1) { builtin_fail(2, L"encrypted", e->name); break; }
        if (e->method != 0 && e->method != 8) { builtin_fail(2, L"a packing method other than stored or deflated", e->name); break; }
        if (e->local + 30 > g_size || !read_at(f, g_offset + e->local, local, 30) || rd32(local) != 0x04034b50) {
            builtin_fail(2, L"no local header", e->name);
            break;
        }
        unsigned long long data_at = e->local + 30 + rd16(local + 26) + rd16(local + 28);
        if (data_at + e->csize > g_size || e->usize > (1ull << 40) || (e->method == 0 && e->csize != e->usize)) {
            builtin_fail(2, L"sizes out of the zip", e->name);
            break;
        }
        unsigned char *packed = HeapAlloc(GetProcessHeap(), 0, (size_t)e->csize + 1);
        unsigned char *plain = e->method == 0 ? packed : HeapAlloc(GetProcessHeap(), 0, (size_t)e->usize + 1);
        int ok = packed && plain;
        if (!ok) builtin_fail(1, L"out of memory", e->name);
        else if (!read_at(f, g_offset + data_at, packed, (size_t)e->csize)) ok = 0, builtin_fail(2, L"could not be read", e->name);
        else if (e->method == 8 && !inflate_all(packed, (size_t)e->csize, plain, (size_t)e->usize))
            ok = 0, builtin_fail(2, L"its packed data is damaged", e->name);
        else if (crc32_of(plain, (size_t)e->usize) != e->crc) ok = 0, builtin_fail(2, L"its CRC-32 differs (damaged)", e->name);
        else if (!write_entry(e, plain, (size_t)e->usize)) ok = 0, builtin_fail(1, L"could not be written", e->name);
        if (plain && plain != packed) HeapFree(GetProcessHeap(), 0, plain);
        if (packed) HeapFree(GetProcessHeap(), 0, packed);
        if (!ok) break;
    }
    CloseHandle(f);
    return 0;
}

/* The zip in this file into g_dir by this program itself, on a few threads. 0, or 1 (writing) / 2 (the zip) with g_why. */
static LONG unzip_builtin(void) {
    wchar_t knob[8];
    DWORD kn = GetEnvironmentVariableW(L"REKARINA_UNPACK_BUILTIN", knob, 8);
    if (kn && kn < 8 && !_wcsicmp(knob, L"off")) {
        wcscpy(g_why, L"not tried (REKARINA_UNPACK_BUILTIN=off)");
        return 2;
    }
    crc_init();
    fixed_init();
    InitializeCriticalSection(&g_why_lock);
    g_why[0] = 0, g_stop = 0, g_fail_kind = 0, g_next = 0;
    wchar_t full[PATH_CHARS];
    DWORD n = GetFullPathNameW(g_dir, PATH_CHARS, full, NULL);
    if (!n || n >= PATH_CHARS) {
        wcscpy(g_why, L"the folder's path");
        return 1;
    }
    for (wchar_t *p = full; *p; p++)
        if (*p == L'/') *p = L'\\';
    if (full[0] == L'\\' && full[1] == L'\\') swprintf(g_full, PATH_CHARS + 8, L"\\\\?\\UNC\\%ls", full + 2);
    else swprintf(g_full, PATH_CHARS + 8, L"\\\\?\\%ls", full);
    HANDLE f = CreateFileW(g_self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    int dir_ok = f != INVALID_HANDLE_VALUE && read_directory(f);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!dir_ok) {
        wcscpy(g_why, L"the zip's directory could not be read (damaged download)");
        return 2;
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int threads = si.dwNumberOfProcessors < 1 ? 1 : si.dwNumberOfProcessors > 16 ? 16 : (int)si.dwNumberOfProcessors;
    HANDLE t[16];
    int k = 0;
    for (int i = 0; i < threads; i++)
        if ((t[k] = CreateThread(NULL, 0, unzip_worker, NULL, 0, NULL))) k++;
    if (!k) unzip_worker(NULL);
    WaitForMultipleObjects((DWORD)k, t, TRUE, INFINITE);
    for (int i = 0; i < k; i++) CloseHandle(t[i]);
    return g_stop ? g_fail_kind : 0;
}

/* tar.exe: the zip out of this file into g_zip, then tar unpacks that into g_dir. 0, or 1 with why. */
static int unpack_tar(wchar_t *why, size_t cap) {
    wchar_t tar[PATH_CHARS], command[PATH_CHARS * 4];
    DWORD k = GetEnvironmentVariableW(L"REKARINA_UNPACK_TAR", tar, PATH_CHARS);
    if (k && k < PATH_CHARS && !_wcsicmp(tar, L"off")) {
        swprintf(why, cap, L"not tried (REKARINA_UNPACK_TAR=off)");
        return 1;
    }
    if (!k || k >= PATH_CHARS) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        swprintf(tar, PATH_CHARS, L"%ls\\tar.exe", sys);
    }
    if (!exists(tar)) {
        swprintf(why, cap, L"%ls is not there", tar);
        return 1;
    }
    LONG result = 1;
    HANDLE in = CreateFileW(g_self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    HANDLE out = CreateFileW(g_zip, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    enum { CHUNK = 1 << 20 };
    unsigned char *buf = HeapAlloc(GetProcessHeap(), 0, CHUNK);
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)g_offset;
    if (in != INVALID_HANDLE_VALUE && out != INVALID_HANDLE_VALUE && buf && SetFilePointerEx(in, at, NULL, FILE_BEGIN)) {
        unsigned long long left = g_size;
        result = 0;
        while (left && !result) {
            DWORD want = left > CHUNK ? CHUNK : (DWORD)left, got = 0, put = 0;
            if (!ReadFile(in, buf, want, &got, NULL) || got != want || !WriteFile(out, buf, got, &put, NULL) || put != got) result = 1;
            left -= got;
        }
    }
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    if (result) swprintf(why, cap, L"the zip could not be copied out to %ls", g_zip);
    else {
        swprintf(command, PATH_CHARS * 4, L"\"%ls\" -xf \"%ls\" -C \"%ls\"", tar, g_zip, g_dir);
        int code = run(command, NULL, 1, CREATE_NO_WINDOW);
        if (code) {
            result = 1;
            if (code < 0) swprintf(why, cap, L"%ls did not start (error %lu)", tar, GetLastError());
            else swprintf(why, cap, L"%ls ended with code %d", tar, code);
        }
    }
    DeleteFileW(g_zip);
    return (int)result;
}

/* Unpacks this file's zip into g_dir: the built-in unpacker, else tar.exe over what it left; then the mark. */
static DWORD WINAPI unpack(void *arg) {
    (void)arg;
    static wchar_t tar_why[PATH_CHARS + 200], built_why[1024], line[PATH_CHARS + 2400], way[1200];
    wcscpy(way, L"the built-in unpacker");
    DWORD t0 = GetTickCount();
    LONG result = unzip_builtin();
    if (!result) {
        swprintf(line, PATH_CHARS + 2400, L"the built-in unpacker: %ld files in %lu ms, every CRC-32 checked", g_nentries,
                 GetTickCount() - t0);
        log_line(line);
    } else {
        wcscpy(built_why, g_why);
        swprintf(line, PATH_CHARS + 2400, L"the built-in unpacker failed: %ls; tar.exe takes over", built_why);
        log_line(line);
        if (unpack_tar(tar_why, PATH_CHARS + 200)) {   /* both failed: the built-in one's kind of failure decides the message */
            swprintf(line, PATH_CHARS + 2400, L"tar.exe: %ls", tar_why);
            log_line(line);
            swprintf(line, PATH_CHARS + 2400, L"the built-in unpacker: %ls; tar.exe: %ls", built_why, tar_why);
            wcsncpy(g_why, line, 1023);
            g_why[1023] = 0;
        } else {
            log_line(L"tar.exe: done");
            swprintf(way, 1200, L"tar.exe (the built-in unpacker: %ls)", built_why);
            result = 0;
        }
    }
    if (!result) {   /* whole: the mark the next start looks for, saying which way */
        wchar_t mark[PATH_CHARS];
        swprintf(mark, PATH_CHARS, L"%ls\\unpacked.txt", g_dir);
        HANDLE m = CreateFileW(mark, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (m == INVALID_HANDLE_VALUE) {
            result = 1;
            swprintf(g_why, 1024, L"%ls could not be written", mark);
        } else {
            static char text[1200 * 3 + 4];
            DWORD put;
            int n = WideCharToMultiByte(CP_UTF8, 0, way, -1, text, sizeof text - 2, NULL, NULL);
            if (n > 1) {
                memcpy(text + n - 1, "\r\n", 2);
                WriteFile(m, text, (DWORD)(n + 1), &put, NULL);
            }
            CloseHandle(m);
        }
    }
    InterlockedExchange(&g_result, result);
    if (g_window) PostMessageW(g_window, WM_CLOSE, 0, 0);
    return 0;
}

static LRESULT CALLBACK window_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CLOSE && g_result < 0) return 0;   /* not while the files are being written */
    if (m == WM_DESTROY) PostQuitMessage(0);
    if (m == WM_CTLCOLORSTATIC) {
        SetTextColor((HDC)wp, RGB(246, 239, 214));
        SetBkColor((HDC)wp, RGB(42, 29, 18));
        return (LRESULT)GetClassLongPtrW(w, GCLP_HBRBACKGROUND);
    }
    return DefWindowProcW(w, m, wp, lp);
}

/* The small window shown while the folder is unpacked: dark wood, cream text, in the middle of the screen. */
static void waiting_window(HINSTANCE inst) {
    WNDCLASSW wc = {.lpfnWndProc = window_proc, .hInstance = inst, .lpszClassName = L"RekarinaSingle",
                    .hbrBackground = CreateSolidBrush(RGB(42, 29, 18)), .hCursor = LoadCursorW(NULL, IDC_WAIT),
                    .hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1))};
    RegisterClassW(&wc);
    int w = 520, h = 150;
    g_window = CreateWindowExW(0, wc.lpszClassName, L"Rekarina", WS_POPUP | WS_CAPTION | WS_VISIBLE,
                               (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                               NULL, NULL, inst, NULL);
    HWND text = CreateWindowExW(0, L"STATIC",
                                russian() ? L"Первый запуск: Rekarina распаковывает свои файлы.\nЭто занимает около минуты и делается один раз."
                                          : L"First start: Rekarina is unpacking its files.\nThis takes about a minute and happens once.",
                                WS_CHILD | WS_VISIBLE | SS_CENTER, 20, 34, w - 56, 60, g_window, NULL, inst, NULL);
    SendMessageW(text, WM_SETFONT, (WPARAM)CreateFontW(-17, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI"), TRUE);
}

static unsigned long long free_gib(const wchar_t *path) {   /* free room on the disk of path, 0 when it cannot be asked */
    wchar_t root[4] = {path[0], L':', L'\\', 0};
    ULARGE_INTEGER avail;
    return path[1] == L':' && GetDiskFreeSpaceExW(root, &avail, NULL, NULL) ? avail.QuadPart >> 30 : 0;
}

/* ---- Rekarina's folder
   Everything of Rekarina lives under one folder the player chooses at the first start: <home>\setup\<version> (this
   program's unpacked files, with the working files of a build in its temp folder) and, as the launcher's first offer
   for the game, <home>\game. The choice is kept in %LOCALAPPDATA%\Rekarina\home.txt (the path, UTF-16), the one thing
   written to the system disk when another disk is chosen. No question is asked when that file names a folder, when
   an earlier version is unpacked in the local application data (that install stays where it is: the folder is then
   %LOCALAPPDATA%\Rekarina), with --home <folder> (a script's choice) or with --unpack-only (no window may come up). */
#define HOME_ROOM_GIB 25   /* the setup 1, a build's working files 9, the game 14 (installer/oot.py NEED_TEMP, NEED_DEST) */

/* A folder as a path ends: no slash, no space and no dot at its end (Windows drops them from a name, so the folder made
   would not be the one the text names), a drive's root as "D:". */
static void tidy_home(wchar_t *p) {
    size_t n = wcslen(p);
    while (n > 2 && (p[n - 1] == L'\\' || p[n - 1] == L'/' || p[n - 1] == L' ' ||
                     (p[n - 1] == L'.' && p[n - 2] != L'\\' && p[n - 2] != L'.' && p[n - 2] != L':')))
        p[--n] = 0;
}

static int read_home(const wchar_t *note, wchar_t *home) {
    HANDLE f = CreateFileW(note, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD got = 0;
    int ok = ReadFile(f, home, (PATH_CHARS - 80) * sizeof(wchar_t), &got, NULL) && got >= 3 * sizeof(wchar_t);
    CloseHandle(f);
    if (!ok) return 0;
    home[got / sizeof(wchar_t)] = 0;
    for (wchar_t *p = home; *p; p++)
        if (*p == L'\r' || *p == L'\n') *p = 0;
    if (home[0] == 0xFEFF) memmove(home, home + 1, (wcslen(home + 1) + 1) * sizeof(wchar_t));   /* an editor's byte order mark */
    tidy_home(home);
    return home[1] == L':' && exists(home);   /* a disk that is gone: no folder, the player is asked again */
}

static void write_home(const wchar_t *note, const wchar_t *home) {
    HANDLE f = CreateFileW(note, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put;
    WriteFile(f, home, (DWORD)(wcslen(home) * sizeof(wchar_t)), &put, NULL);
    CloseHandle(f);
}

/* Windows' own folder dialog, opened at `start`. 1 with the folder in out, 0 when the player closed it. */
static int pick_folder(const wchar_t *start, wchar_t *out) {
    IFileOpenDialog *d = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d))) return 0;
    DWORD options = 0;
    IFileOpenDialog_GetOptions(d, &options);
    IFileOpenDialog_SetOptions(d, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    IFileOpenDialog_SetTitle(d, russian() ? L"Rekarina: папка для лаунчера и игры (нужно около 25 ГБ)"
                                          : L"Rekarina: a folder for the launcher and the game (about 25 GB)");
    IFileOpenDialog_SetOkButtonLabel(d, russian() ? L"Поставить сюда" : L"Install here");
    IShellItem *at = NULL;
    if (start && SUCCEEDED(SHCreateItemFromParsingName(start, NULL, &IID_IShellItem, (void **)&at))) {
        IFileOpenDialog_SetFolder(d, at);
        IShellItem_Release(at);
    }
    IShellItem *item = NULL;
    if (SUCCEEDED(IFileOpenDialog_Show(d, NULL)) && SUCCEEDED(IFileOpenDialog_GetResult(d, &item))) {
        wchar_t *path = NULL;
        if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path)) && path) {
            ok = wcslen(path) < PATH_CHARS - 120 && path[1] == L':';
            if (ok) wcscpy(out, path);
            CoTaskMemFree(path);
        }
        IShellItem_Release(item);
    }
    IFileOpenDialog_Release(d);
    return ok;
}

static int folder_is_empty(const wchar_t *folder) {
    wchar_t all[PATH_CHARS];
    WIN32_FIND_DATAW fd;
    swprintf(all, PATH_CHARS, L"%ls\\*", folder);
    HANDLE h = FindFirstFileW(all, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    int empty = 1;
    do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) empty = 0;
    } while (empty && FindNextFileW(h, &fd));
    FindClose(h);
    return empty;
}

static int names_rekarina(const wchar_t *folder) {   /* the folder's own name has "rekarina" in it, in any case */
    const wchar_t *leaf = wcsrchr(folder, L'\\');
    wchar_t low[64];
    int n = 0;
    for (leaf = leaf ? leaf + 1 : folder; *leaf && n < 63; leaf++) low[n++] = (wchar_t)towlower(*leaf);
    low[n] = 0;
    return wcsstr(low, L"rekarina") != NULL;
}

/* home: Rekarina's folder, without a slash at its end. *asked: the player has just chosen it (the launcher is then
   given <home>\game as the game folder to offer). Ends the program when the player closes the question. */
static void find_home(const wchar_t *appdata, const wchar_t *args, int quiet, wchar_t *home, int *asked) {
    wchar_t note[PATH_CHARS], old[PATH_CHARS], here[PATH_CHARS];
    *asked = 0;
    swprintf(home, PATH_CHARS, L"%ls\\Rekarina", appdata);
    swprintf(note, PATH_CHARS, L"%ls\\home.txt", home);
    swprintf(old, PATH_CHARS, L"%ls\\setup", home);
    const wchar_t *given = args ? wcsstr(args, L"--home ") : NULL;
    if (given) {   /* --home <folder>, the folder in quotes when it has spaces */
        given += 7;
        while (*given == L' ') given++;
        wchar_t stop = *given == L'"' ? L'"' : L' ';
        if (*given == L'"') given++;
        int n = 0;
        while (given[n] && given[n] != stop && n < PATH_CHARS - 120) n++;
        if (n > 2 && n < PATH_CHARS - 120 && given[1] == L':') {   /* a value too long for the buffers is not taken cut */
            wcsncpy(home, given, n);
            home[n] = 0;
            tidy_home(home);
        }
        return;
    }
    int again = !quiet && args && wcsstr(args, L"--choose") != NULL;   /* --choose: ask even where a folder is known */
    if (!again && read_home(note, here)) {
        wcscpy(home, here);
        return;
    }
    if (!again && (exists(old) || quiet)) return;   /* an install made before the choice existed, or no window allowed */
    wcscpy(here, g_self);
    wchar_t *cut = wcsrchr(here, L'\\');
    if (cut) *cut = 0;
    for (;;) {
        wchar_t picked[PATH_CHARS];
        if (!pick_folder(here, picked)) ExitProcess(0);
        size_t n = wcslen(picked);
        if (n > 3 && picked[n - 1] == L'\\') picked[n - 1] = 0;
        /* a folder that holds other things gets a folder of Rekarina's own inside it */
        if (wcslen(picked) <= 3 || (!folder_is_empty(picked) && !names_rekarina(picked))) {
            if (picked[wcslen(picked) - 1] == L'\\') picked[wcslen(picked) - 1] = 0;
            wcscat(picked, L"\\Rekarina");
        }
        unsigned long long room = free_gib(picked);
        if (room < HOME_ROOM_GIB) {
            static wchar_t text[PATH_CHARS + 500];
            swprintf(text, PATH_CHARS + 500, russian()
                     ? L"На диске %lc: свободно %llu ГБ, а Rekarina вместе с игрой нужно около %d ГБ.\n\nВыбрать другую папку?"
                     : L"Disk %lc: has %llu GB free, and Rekarina with the game needs about %d GB.\n\nChoose another folder?",
                     (wchar_t)towupper(picked[0]), room, HOME_ROOM_GIB);
            if (MessageBoxW(NULL, text, L"Rekarina", MB_YESNO | MB_ICONWARNING) == IDYES) continue;
        }
        wcscpy(home, picked);
        break;
    }
    wchar_t folder[PATH_CHARS];
    swprintf(folder, PATH_CHARS, L"%ls\\Rekarina", appdata);
    make_dirs(folder);
    write_home(note, home);
    *asked = 1;
}

/* This file is the game folder's Rekarina.exe: the folder holds rekarina-install.json, and this program has the game's
   name. Saved there (a download into the game folder, or over the game), it took the game's place, and the launcher's
   Play started this program again instead of the game (2026-11-20). The player is told, and the launcher opens on that
   folder, where Play puts the game back (installer/gui_oot.py repair_game; the package step moves this file aside as
   Rekarina-download.exe). 1 with the folder in `game`. */
static int in_game_folder(wchar_t *game) {
    wcscpy(game, g_self);
    wchar_t *leaf = wcsrchr(game, L'\\');
    if (!leaf || _wcsicmp(leaf + 1, L"Rekarina.exe")) return 0;
    *leaf = 0;
    wchar_t record[PATH_CHARS];
    swprintf(record, PATH_CHARS, L"%ls\\rekarina-install.json", game);
    if (!exists(record)) return 0;
    static wchar_t text[PATH_CHARS + 1024];
    swprintf(text, PATH_CHARS + 1024, russian()
             ? L"Этот Rekarina.exe — скачанный файл: установщик и лаунчер в одном файле. Он лежит в папке игры\n%ls\n"
               L"на месте Rekarina.exe самой игры.\n\nСейчас откроется лаунчер: «Играть» вернёт игру на место. "
               L"Держите скачанный файл в другой папке, например в «Загрузках»."
             : L"This Rekarina.exe is the download: the setup and the launcher in one file. It is in the game folder\n%ls\n"
               L"in the place of the game's own Rekarina.exe.\n\nThe launcher opens now: Play puts the game back. "
               L"Keep the download in another folder, such as Downloads.",
             game);
    MessageBoxW(NULL, text, L"Rekarina", MB_OK | MB_ICONWARNING);
    return 1;
}

/* The arguments the launcher gets: this program's own (--choose, --home <folder>) taken out of a copy of args. */
static void launcher_args(const wchar_t *args, wchar_t *out, size_t cap) {
    out[0] = 0;
    if (!args) return;
    wcsncpy(out, args, cap - 1);
    out[cap - 1] = 0;
    wchar_t *p = wcsstr(out, L"--choose");
    if (p) memmove(p, p + 8, (wcslen(p + 8) + 1) * sizeof(wchar_t));
    p = wcsstr(out, L"--home ");
    if (p) {
        wchar_t *q = p + 7;
        wchar_t stop = *q == L'"' ? L'"' : L' ';
        if (*q == L'"') q++;
        while (*q && *q != stop) q++;
        if (*q == L'"') q++;
        memmove(p, q, (wcslen(q) + 1) * sizeof(wchar_t));
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, wchar_t *args, int show) {
    (void)prev, (void)show;
    wchar_t base[PATH_CHARS], home[PATH_CHARS], launcher[PATH_CHARS], mark[PATH_CHARS];
    int asked = 0;
    int unpack_only = args && wcsstr(args, L"--unpack-only");
    g_quiet = unpack_only;
    if (!GetModuleFileNameW(NULL, g_self, PATH_CHARS) || !read_trailer(g_version, 40))
        fail(L"This copy of Rekarina.exe is incomplete. Download it again.",
             L"Эта копия Rekarina.exe неполная. Скачайте её заново.");
    DWORD base_chars = GetEnvironmentVariableW(L"LOCALAPPDATA", base, PATH_CHARS - 120);
    if (base_chars == 0 || base_chars >= PATH_CHARS - 120)   /* unset, or too long for the buffer (the call then writes nothing) */
        fail(L"Windows did not say where local application data goes (LOCALAPPDATA).",
             L"Windows не сообщила, где хранить данные приложений (LOCALAPPDATA).");
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   /* the folder dialog of find_home */
    find_home(base, args, unpack_only, home, &asked);
    swprintf(g_dir, PATH_CHARS, L"%ls\\setup\\%ls", home, g_version);
    swprintf(g_zip, PATH_CHARS, L"%ls.zip", g_dir);
    swprintf(launcher, PATH_CHARS, L"%ls\\Rekarina-Launcher.exe", g_dir);
    swprintf(mark, PATH_CHARS, L"%ls\\unpacked.txt", g_dir);
    if (!exists(mark) || !exists(launcher)) {
        if (!make_dirs(g_dir))
            fail(L"Rekarina could not make the folder for its files.", L"Rekarina не смогла создать папку для своих файлов.");
        if (unpack_only) {   /* no window: nothing may come up on the screen of a test or a script */
            unpack(NULL);
        } else {
            waiting_window(inst);
            HANDLE thread = CreateThread(NULL, 0, unpack, NULL, 0, NULL);
            if (!thread) fail(L"Rekarina could not start unpacking.", L"Rekarina не смогла начать распаковку.");
            MSG msg;
            while (GetMessageW(&msg, NULL, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
        static wchar_t en[1600], ru[1600];
        if (g_result == 1) {
            swprintf(en, 1600, L"Rekarina could not write its files. Check the free space on the disk they go to (about 1 GB "
                               L"is needed) and that no antivirus blocks the folder.\n\n%ls", g_why);
            swprintf(ru, 1600, L"Rekarina не смогла записать свои файлы. Проверьте свободное место на диске, куда они "
                               L"пишутся (нужно около 1 ГБ), и что антивирус не закрывает папку.\n\n%ls", g_why);
            fail(en, ru);
        }
        if (g_result) {
            swprintf(en, 1600, L"Rekarina could not unpack its files: this download may be damaged. Download Rekarina.exe "
                               L"again.\n\n%ls", g_why);
            swprintf(ru, 1600, L"Rekarina не смогла распаковать свои файлы: скачанный файл, возможно, повреждён. Скачайте "
                               L"Rekarina.exe заново.\n\n%ls", g_why);
            fail(en, ru);
        }
    }
    if (unpack_only) return 0;   /* a test, or a setup script: the folder alone */
    wchar_t command[PATH_CHARS + 4096], game[PATH_CHARS];
    /* the game folder the launcher offers after the choice: beside the setup, under the same folder */
    static wchar_t rest[4096];
    launcher_args(args, rest, 4096);
    if (in_game_folder(game)) swprintf(command, PATH_CHARS + 4096, L"\"%ls\" --dest \"%ls\" %ls", launcher, game, rest);
    else if (asked) swprintf(command, PATH_CHARS + 4096, L"\"%ls\" --dest \"%ls\\game\" %ls", launcher, home, rest);
    else swprintf(command, PATH_CHARS + 4096, L"\"%ls\" %ls", launcher, rest);
    if (run(command, g_dir, 0, 0) < 0)
        fail(L"Rekarina's launcher did not start.", L"Лаунчер Rekarina не запустился.");
    return 0;
}
