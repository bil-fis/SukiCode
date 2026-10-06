// SukiCode minimal C runtime.
//
// Provides the primitives the compiler-generated code and the bootstrap
// standard library rely on: printing, panic, and aligned allocation.
// The object-code stage links this runtime with the user's translation unit.
//
// ARC reference counting (suki_arc_retain/release) uses acquire/release
// memory ordering; only debug counters use relaxed ordering.

// On POSIX, expose `nanosleep` (needs _POSIX_C_SOURCE >= 199309L) *before*
// any system header is pulled in (including transitively via runtime.h),
// otherwise its declaration stays hidden and we get an implicit-declaration
// warning (C11 UB). Feature-test macros are only consulted at the first
// system-header inclusion, so this MUST precede every #include.
#if !defined(_WIN32)
  #define _POSIX_C_SOURCE 199309L
#endif

#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>
#if !defined(_WIN32)
  #include <unistd.h>   // nanosleep (POSIX)
#endif

// ─── platform threading backend ────────────────────────────────────────────────
// Windows: Win32 threads, critical sections, condition variables.
// Linux / macOS / other POSIX: pthreads.
// Everything below is written against these two typedefs, so the rest of the
// runtime stays platform-neutral.
#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <process.h>
#else
  #include <pthread.h>
#endif

// ─── integer-overflow-safe size arithmetic ───────────────────────────────
// Allocation sizes are products of caller-supplied counts and element sizes.
// A naive `a * b` can wrap, yielding a small size_t that leads to a heap
// buffer overflow. These helpers detect overflow and abort instead of
// passing a wrong size to malloc.
static int suki_size_mul(int64_t a, int64_t b, size_t* out) {
    if (a < 0 || b < 0) return 1;          // negative counts are never valid
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
    if (ua != 0 && ub > UINT64_MAX / ua) return 1;
    uint64_t prod = ua * ub;
    if (prod > (uint64_t)SIZE_MAX) return 1;
    *out = (size_t)prod;
    return 0;
}

// Round up to the next power of two (used for the dictionary capacity so the
// probe mask `cap - 1` is always a full bitmask). Returns 0 on overflow.
static int64_t suki_round_pow2(int64_t v) {
    if (v <= 1) return 1;
    if (v > ((int64_t)1 << 62)) return 0;  // would overflow int64
    int64_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

// ─── printing ─────────────────────────────────────────────────────────────
void print(const char* s) {
    if (s) fputs(s, stdout);
}

void println(const char* s) {
    if (s) fputs(s, stdout);
    fputc('\n', stdout);
}

// ─── panic ─────────────────────────────────────────────────────────────────
void panic(const char* msg) {
    if (msg) fprintf(stderr, "panic: %s\n", msg);
    else fputs("panic\n", stderr);
    abort();
}

// ─── allocation ───────────────────────────────────────────────────────────
void* suki_alloc(size_t size) {
    void* p = malloc(size ? size : 1);
    if (!p) panic("out of memory");
    return p;
}

void suki_free(void* p) { free(p); }

// ─── ARC reference counting ────────────────────────────────────────────────
// Object header layout, fixed by the code generator (see TypeLayout):
//
//     offset 0 : vtable*      (dynamic dispatch table)
//     offset 8 : int64 rc     (strong reference count)
//     offset 16: i8* deinit   (destructor entry, or null)
//     offset 24: first instance field
//
// The counter therefore lives at offset 8, *not* at offset 0 — offset 0 holds
// the vtable pointer. Both this file and TypeLayout must agree on these offsets.
//
// Acquire/release pairing (never relaxed) so a retain/release on one thread is
// properly ordered against the destructor on another.
#define SUKI_OBJECT_RC_OFFSET 8
// Destructor slot: run when the last strong reference disappears.
#define SUKI_OBJECT_DEINIT_OFFSET 16

// A null reference owns nothing, so both operations accept it and report a
// count of 0. Without this guard, releasing a field that was never assigned
// (its storage is zero-initialised) would dereference address 8.
uint64_t suki_arc_retain(void* obj) {
    if (!obj) return 0;
    uint64_t* counter = (uint64_t*)((char*)obj + SUKI_OBJECT_RC_OFFSET);
    return __atomic_fetch_add(counter, 1, __ATOMIC_ACQ_REL) + 1;
}

// Destructor signature stored in the object header. It receives the object so
// `deinit` can still reach its fields while running.
typedef void (*SukiDeinit)(void*);

uint64_t suki_arc_release(void* obj) {
    if (!obj) return 0;
    uint64_t* counter = (uint64_t*)((char*)obj + SUKI_OBJECT_RC_OFFSET);
    // acquire on the decrement: synchronizes with the retain that produced it.
    uint64_t remaining = __atomic_sub_fetch(counter, 1, __ATOMIC_ACQ_REL);
    if (remaining == 0) {
        // Last strong reference: clear every registered weak slot pointing at
        // this object *before* running `deinit` and freeing, so that any
        // outstanding `weak` reference observes nil instead of a dangling
        // pointer (spec §6.1: weak references auto-nil on deallocation).
        suki_weak_clear_all(obj);
        // Run `deinit` before releasing the storage. The slot is null for
        // classes that declare none; a subclass that does not declare its own
        // simply inherits the pointer the constructor stored.
        SukiDeinit dtor =
            (SukiDeinit)*(void**)((char*)obj + SUKI_OBJECT_DEINIT_OFFSET);
        if (dtor) dtor(obj);
        suki_free(obj);
    }
    return remaining;
}

// ─── String ────────────────────────────────────────────────────────────────
// Layout: { const char* data; int64_t length } — identical to the LLVM
// aggregate the compiler builds, so values cross this boundary unchanged.
SukiString suki_str_lit(const char* utf8, int64_t len) {
    SukiString s;
    s.data = utf8 ? utf8 : "";
    s.length = utf8 ? len : 0;
    return s;
}

SukiString suki_str_from_cstr(const char* z) {
    SukiString s;
    if (!z) { s.data = ""; s.length = 0; return s; }
    s.data = z;
    s.length = (int64_t)strlen(z);
    return s;
}

// ─── Time ────────────────────────────────────────────────────────────────────────
// Safe wrappers around libc time functions. The SukiCode side declares these as
// parameterless foreign functions returning Int, so no pointer-sized argument
// crosses the FFI boundary (avoids an ABI quirk where the compiler does not
// reliably place the NULL pointer argument for time(NULL)/clock()).
int64_t suki_time_now_sec(void) {
    time_t t = time(NULL);
    return (int64_t)t;
}
int64_t suki_clock_cpu_micros(void) {
    // clock(3) is unreliable in some environments (returns 0 / garbage); use a
    // monotonic clock via clock_gettime for a stable, monotonically increasing
    // microsecond timer suitable for benchmarking / elapsed-time measurement.
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)ts.tv_nsec / 1000;
}

int64_t suki_str_length(SukiString s) { return s.length; }

const char* suki_str_data(SukiString s) { return s.data ? s.data : ""; }

SukiString suki_str_concat(SukiString a, SukiString b) {
    if (a.length < 0) a.length = 0;
    if (b.length < 0) b.length = 0;
    if (a.length > INT64_MAX - b.length) panic("string concatenation overflow");
    int64_t n = a.length + b.length;
    char* buf = (char*)suki_alloc((size_t)n + 1);
    if (a.data && a.length) memcpy(buf, a.data, (size_t)a.length);
    if (b.data && b.length) memcpy(buf + a.length, b.data, (size_t)b.length);
    buf[n] = '\0';
    SukiString r = { buf, n };
    return r;
}

int32_t suki_str_compare(SukiString a, SukiString b) {
    // Length-first is the common fast path; fall back to a bounded memcmp so
    // embedded NUL bytes compare correctly rather than truncating.
    int64_t n = a.length < b.length ? a.length : b.length;
    int c = n ? memcmp(a.data ? a.data : "", b.data ? b.data : "", (size_t)n) : 0;
    if (c) return c < 0 ? -1 : 1;
    if (a.length == b.length) return 0;
    return a.length < b.length ? -1 : 1;
}

// Debug representation used when a value that has no built-in textual form (an
// enum, struct, class, tuple, ...) is interpolated into a string literal. Without
// this the compiler would pass the raw aggregate value where a String is expected,
// producing a type mismatch in the IR verifier. We return "<TypeName>" instead.
SukiString suki_debug_repr(const char* t) {
    if (!t) { SukiString s = { (char*)"", 0 }; return s; }
    size_t n = strlen(t);
    char* buf = (char*)suki_alloc(n + 3);
    buf[0] = '<';
    if (n) memcpy(buf + 1, t, n);
    buf[n + 1] = '>';
    buf[n + 2] = '\0';
    SukiString s = { buf, (int64_t)(n + 2) };
    return s;
}

// Returns the byte length of the UTF-8 sequence starting at `p`.
static int32_t suki_utf8_seq_len(const unsigned char* p) {
    unsigned char c = *p;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1; // invalid lead byte: consume one to guarantee progress
}

int32_t suki_str_has_prefix(SukiString s, SukiString prefix) {
    if (prefix.length > s.length) return 0;
    if (prefix.length == 0) return 1;
    const char* sd = s.data ? s.data : "";
    const char* pd = prefix.data ? prefix.data : "";
    return memcmp(sd, pd, (size_t)prefix.length) == 0;
}

int32_t suki_str_has_suffix(SukiString s, SukiString suffix) {
    if (suffix.length > s.length) return 0;
    if (suffix.length == 0) return 1;
    const char* sd = s.data ? s.data : "";
    const char* pxd = suffix.data ? suffix.data : "";
    return memcmp(sd + (s.length - suffix.length), pxd,
                  (size_t)suffix.length) == 0;
}

int32_t suki_str_utf8_count(SukiString s) {    int32_t n = 0;
    if (!s.data) return 0;
    for (int64_t i = 0; i < s.length; ) {
        int32_t w = suki_utf8_seq_len((const unsigned char*)s.data + i);
        i += w;
        ++n;
    }
    return n;
}

// Returns the Unicode scalar at `index` (character, not byte), or -1 when the
// index is out of range. This is the backing for SukiCode's String view.
int32_t suki_str_utf8_get(SukiString s, int32_t index) {
    if (!s.data) return -1;
    int64_t i = 0;
    int32_t n = 0;
    while (i < s.length) {
        const unsigned char* p = (const unsigned char*)s.data + i;
        int32_t w = suki_utf8_seq_len(p);
        // A truncated lead byte claims more bytes than remain before the end
        // of the string; reading them would go past the buffer. Treat a
        // truncated sequence at the requested index as "out of range".
        if ((int64_t)w > s.length - i) return -1;
        if (n == index) {
            switch (w) {
                case 1: return p[0];
                case 2: return ((int32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
                case 3: return ((int32_t)(p[0] & 0x0F) << 12) |
                               ((int32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
                default: return ((int32_t)(p[0] & 0x07) << 18) |
                                ((int32_t)(p[1] & 0x3F) << 12) |
                                ((int32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            }
        }
        i += w;
        ++n;
    }
    return -1;
}

// ─── interpolation formatting ──────────────────────────────────────────────
// These results outlive the call (they are concatenated into a String that the
// caller owns), so the buffer must be heap-allocated rather than a local array.
// Renders one Unicode scalar as a String: ASCII is a single byte, anything
// else is UTF-8 encoded (最多 4 字节)。
// Parses a String as an integer. The bytes are copied into a NUL-terminated
// buffer first because the runtime string carries an explicit length and may
// contain embedded NULs.
int64_t suki_str_to_int(SukiString s) {
    char buf[64];
    int64_t n = s.length < 63 ? s.length : 63;
    if (n < 0) n = 0;
    if (!s.data) n = 0;
    memcpy(buf, s.data, (size_t)n);
    buf[n] = '\0';
    return (int64_t)strtoll(buf, NULL, 10);
}

double suki_str_to_double(SukiString s) {
    char buf[64];
    int64_t n = s.length < 63 ? s.length : 63;
    if (n < 0) n = 0;
    if (!s.data) n = 0;
    memcpy(buf, s.data, (size_t)n);
    buf[n] = '\0';
    return strtod(buf, NULL);
}

SukiString suki_char_to_string(int32_t v) {
    char buf[5];
    int64_t n = 0;
    uint32_t cp = (uint32_t)v;
    if (cp < 0x80) {
        buf[n++] = (char)cp;
    } else if (cp < 0x800) {
        buf[n++] = (char)(0xC0 | (cp >> 6));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        buf[n++] = (char)(0xE0 | (cp >> 12));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[n++] = (char)(0xF0 | (cp >> 18));
        buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    }
    // `suki_str_lit` only stores the pointer it is given, so the stack buffer
    // above must be copied to the heap — returning it directly would leave the
    // caller with a dangling String as soon as this frame is popped.
    char* out = (char*)suki_alloc((size_t)n + 1);
    if (!out) { SukiString empty = { "", 0 }; return empty; }
    memcpy(out, buf, (size_t)n);
    out[n] = '\0';
    SukiString s = { out, n };
    return s;
}

SukiString suki_int_to_string(int64_t v) {
    char buf[24];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    if (n < 0) n = 0;
    char* out = (char*)suki_alloc((size_t)n + 1);
    memcpy(out, buf, (size_t)n);
    out[n] = '\0';
    SukiString s = { out, n };
    return s;
}

SukiString suki_double_to_string(double v) {
    char buf[40];
    // %g keeps integral results readable ("5" rather than "5.000000") while
    // still showing fractions when they are meaningful.
    int n = snprintf(buf, sizeof buf, "%g", v);
    if (n < 0) n = 0;
    char* out = (char*)suki_alloc((size_t)n + 1);
    memcpy(out, buf, (size_t)n);
    out[n] = '\0';
    SukiString s = { out, n };
    return s;
}

void suki_print_str(SukiString s) {
    if (s.data && s.length > 0) fwrite(s.data, 1, (size_t)s.length, stdout);
}

void suki_println_str(SukiString s) {
    suki_print_str(s);
    fputc('\n', stdout);
}

// ABI-compatible panic that takes a SukiCode `String` by value (the same
// {i8* data, i64 length} aggregate the compiler uses), so the standard library
// can trap without reaching for the compiler builtin `panic` (which is only
// visible inside the main module, not inside imported modules such as `core`).
void suki_panic_string(SukiString s) {
    fputs("panic: ", stderr);
    if (s.data && s.length > 0) fwrite(s.data, 1, (size_t)s.length, stderr);
    fputc('\n', stderr);
    abort();
}

// ─── Array ────────────────────────────────────────────────────────────────
// Storage is a single block: [ int64 elem_size | capacity * elem_size bytes ].
// `data` points just past the header, so element i is `data[i]` for any type.
void suki_array_new(int64_t elem_size, int64_t capacity, SukiArray* out) {
    if (!out) return;
    if (elem_size <= 0) elem_size = 1;
    if (capacity < 0) capacity = 0;
    size_t bytes;
    if (suki_size_mul(capacity, elem_size, &bytes)) panic("array size overflow");
    size_t total = bytes + sizeof(int64_t);
    if (total < bytes) panic("array size overflow");
    char* block = (char*)suki_alloc(total ? total : 1);
    *(int64_t*)block = elem_size;
    out->data = block + sizeof(int64_t);
    out->length = 0;
    out->capacity = capacity;
}

int64_t suki_array_len(const SukiArray* a) { return a ? a->length : 0; }

static int64_t suki_array_elem_size(SukiArray a) {
    if (!a.data) return 1;
    return *(const int64_t*)((const char*)a.data - sizeof(int64_t));
}

void suki_array_push(SukiArray* ap, int64_t elem_size, const void* value) {
    if (!ap) return;
    SukiArray a = *ap;
    if (elem_size <= 0) elem_size = 1;
    if (a.length == a.capacity) {
        // Grow geometrically; the length is carried in the first slot of the
        // block header so a by-value SukiArray still sees the update.
        int64_t newCap = a.capacity ? a.capacity * 2 : 4;
        if (newCap <= a.capacity) panic("array capacity overflow");
        size_t body;
        if (suki_size_mul(newCap, elem_size, &body)) panic("array resize overflow");
        char* newBlock = (char*)suki_alloc(sizeof(int64_t) + (body ? body : 1));
        *(int64_t*)newBlock = elem_size;
        if (a.length)
            memcpy(newBlock + sizeof(int64_t), a.data, (size_t)(a.length * elem_size));
        a.data = newBlock + sizeof(int64_t);
        a.capacity = newCap;
    }
    memcpy((char*)a.data + a.length * elem_size, value, (size_t)elem_size);
    a.length += 1;
    *ap = a; // growth is reported back through the caller's aggregate
}

void* suki_array_get(const SukiArray* a, int64_t index) {
    if (!a || index < 0 || index >= a->length) return NULL;
    return (char*)a->data + index * suki_array_elem_size(*a);
}

void suki_array_set(const SukiArray* a, int64_t index, const void* value) {
    if (!a || index < 0 || index >= a->length) return;
    int64_t esz = suki_array_elem_size(*a);
    memcpy((char*)a->data + index * esz, value, (size_t)esz);
}

void suki_array_remove_at(SukiArray* ap, int64_t index) {
    if (!ap) return;
    SukiArray a = *ap;
    if (index < 0 || index >= a.length) return;
    int64_t esz = suki_array_elem_size(a);
    char* base = (char*)a.data;
    // Shift the tail down, then drop the last element.
    if (index < a.length - 1)
        memmove(base + index * esz, base + (index + 1) * esz,
                (size_t)((a.length - 1 - index) * esz));
    a.length -= 1;
    *ap = a;
}

void suki_array_free(SukiArray a) {
    if (a.data) suki_free((char*)a.data - sizeof(int64_t));
}

// ─── Dictionary / Set ──────────────────────────────────────────────────────
// One open-addressing table with linear probing, shared by Dictionary and Set.
//
// Block layout (all offsets in bytes from `data`):
//     0                : int64 key_size
//     8                : int64 val_size
//     16               : capacity * (key_size + 1 + val_size)
// Each slot stores the raw key bytes, an occupancy byte, then the value bytes.
// Linear probing keeps lookups allocation-free; a tombstone (0xFF) marks a
// removed entry so probe chains stay intact.
#define SUKI_DICT_HDR      16
#define SUKI_DICT_EMPTY    0x00
#define SUKI_DICT_FULL     0x01
#define SUKI_DICT_TOMBSTONE 0xFF
#define SUKI_DICT_MIN_CAP  8

// FNV-1a: cheap, well-distributed, and needs no per-type knowledge.
static uint64_t suki_dict_hash(const void* key, int64_t len) {
    const unsigned char* p = (const unsigned char*)key;
    uint64_t h = 1469598103934665603ULL;
    for (int64_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// Resolve the header fields stored in front of the slot table.
typedef struct {
    int64_t key_size;
    int64_t val_size;
    int64_t slot_size;
    unsigned char* base;   // first slot
} SukiDictView;

static SukiDictView suki_dict_view(const SukiDict* d) {
    SukiDictView v;
    v.key_size = d && d->data ? *(const int64_t*)d->data : 0;
    v.val_size = d && d->data ? *(const int64_t*)((const char*)d->data + 8) : 0;
    v.slot_size = v.key_size + 1 + v.val_size;
    v.base = d && d->data ? (unsigned char*)d->data + SUKI_DICT_HDR : NULL;
    return v;
}

static unsigned char* suki_dict_slot(const SukiDictView* v, int64_t i) {
    return v->base + i * v->slot_size;
}

void suki_dict_new(int64_t key_size, int64_t val_size, int64_t capacity,
                   SukiDict* out) {
    if (!out) return;
    if (key_size <= 0) key_size = 1;
    if (val_size < 0) val_size = 0;
    if (capacity < SUKI_DICT_MIN_CAP) capacity = SUKI_DICT_MIN_CAP;
    capacity = suki_round_pow2(capacity);
    if (capacity == 0) panic("dictionary capacity overflow");
    // `slot` is fixed per table; `capacity` is now a power of two so the probe
    // mask `cap - 1` stays correct. Guard both multiplications against overflow.
    int64_t slot = key_size + 1 + val_size;
    if (slot <= 0) panic("invalid dictionary key/value size");
    size_t tableBytes;
    if (suki_size_mul(capacity, slot, &tableBytes)) panic("dictionary size overflow");
    size_t total = (size_t)SUKI_DICT_HDR + tableBytes;
    if (total < tableBytes) panic("dictionary size overflow");
    char* block = (char*)suki_alloc(total);
    *(int64_t*)block = key_size;
    *(int64_t*)(block + 8) = val_size;
    // Use the already-overflow-checked byte count, not a fresh `capacity*slot`
    // (which is int64 and can wrap even when the size_t product is valid).
    memset(block + SUKI_DICT_HDR, SUKI_DICT_EMPTY, tableBytes);
    out->data = block;
    out->count = 0;
    out->capacity = capacity;
}

int64_t suki_dict_len(const SukiDict* d) { return d ? d->count : 0; }

// Locate the slot holding `key`, or NULL when absent. When `for_insert` is
// set, the first tombstone met on the probe chain is reused, so that deleting
// an entry and reinserting a different key never splits the chain.
// `cap` is always a power of two, which lets the modulo become a mask.
static unsigned char* suki_dict_probe(const SukiDictView* v, int64_t cap,
                                      const void* key, int for_insert) {
    if (!v->base || cap <= 0) return NULL;
    unsigned char* first_tomb = NULL;
    uint64_t start = suki_dict_hash(key, v->key_size) & (uint64_t)(cap - 1);
    for (uint64_t step = 0; step < (uint64_t)cap; ++step) {
        unsigned char* s = suki_dict_slot(v, (int64_t)((start + step) & (uint64_t)(cap - 1)));
        unsigned char state = s[v->key_size];
        if (state == SUKI_DICT_FULL) {
            if (memcmp(s, key, (size_t)v->key_size) == 0) return s;
        } else if (state == SUKI_DICT_TOMBSTONE) {
            if (!first_tomb) first_tomb = s;
        } else { // EMPTY: the probe chain ends here
            return for_insert ? (first_tomb ? first_tomb : s) : NULL;
        }
    }
    return for_insert ? first_tomb : NULL;
}

// Grow when the table is 70% full, preserving existing entries.
static void suki_dict_grow(SukiDict* d) {
    SukiDictView old = suki_dict_view(d);
    int64_t oldCap = d->capacity;
    if (!old.base) return;
    // Copy slot payloads aside before releasing the old block.
    int64_t newCap = oldCap * 2;
    if (newCap <= oldCap) panic("dictionary capacity overflow");
    SukiDict fresh;
    suki_dict_new(old.key_size, old.val_size, newCap, &fresh);
    SukiDictView nv = suki_dict_view(&fresh);
    for (int64_t i = 0; i < oldCap; ++i) {
        unsigned char* s = suki_dict_slot(&old, i);
        if (s[old.key_size] != SUKI_DICT_FULL) continue;
        unsigned char* dst = suki_dict_probe(&nv, fresh.capacity, s, 1);
        memcpy(dst, s, (size_t)old.key_size);
        dst[old.key_size] = SUKI_DICT_FULL;
        if (old.val_size) memcpy(dst + old.key_size + 1,
                                 s + old.key_size + 1, (size_t)old.val_size);
        fresh.count += 1;
    }
    suki_free(d->data);
    *d = fresh;
}

int32_t suki_dict_get(const SukiDict* d, int64_t key_size, int64_t val_size,
                      const void* key, void* out_value) {
    (void)key_size; (void)val_size;
    SukiDictView v = suki_dict_view(d);
    unsigned char* s = suki_dict_probe(&v, d ? d->capacity : 0, key, 0);
    if (!s) return 0;
    if (out_value && v.val_size)
        memcpy(out_value, s + v.key_size + 1, (size_t)v.val_size);
    return 1;
}

void suki_dict_set(SukiDict* d, int64_t key_size, int64_t val_size,
                   const void* key, const void* value) {
    (void)key_size; (void)val_size;
    if (!d || !d->data) return;
    // Keep the load factor below 0.7 so probe chains stay short.
    if ((d->count + 1) * 10 >= d->capacity * 7) suki_dict_grow(d);
    SukiDictView v = suki_dict_view(d);
    unsigned char* s = suki_dict_probe(&v, d->capacity, key, 1);
    if (!s) {
        // Table was somehow full (e.g. exact-size capacity with no headroom):
        // grow once and retry rather than silently dropping the insertion.
        suki_dict_grow(d);
        v = suki_dict_view(d);
        s = suki_dict_probe(&v, d->capacity, key, 1);
        if (!s) return;
    }
    int wasFull = (s[v.key_size] == SUKI_DICT_FULL);
    if (!wasFull) d->count += 1;
    memcpy(s, key, (size_t)v.key_size);
    s[v.key_size] = SUKI_DICT_FULL;
    if (v.val_size && value) memcpy(s + v.key_size + 1, value, (size_t)v.val_size);
}

int32_t suki_dict_remove(SukiDict* d, int64_t key_size, int64_t val_size,
                        const void* key) {
    (void)key_size; (void)val_size;
    if (!d || !d->data) return 0;
    SukiDictView v = suki_dict_view(d);
    unsigned char* s = suki_dict_probe(&v, d->capacity, key, 0);
    if (!s) return 0;
    // Tombstone rather than EMPTY: an EMPTY marker would truncate this probe
    // chain and make later keys unreachable.
    s[v.key_size] = SUKI_DICT_TOMBSTONE;
    d->count -= 1;
    return 1;
}

// ─── dictionary iteration ─────────────────────────────────────────────
// Walks the slot table in order and hands back the i-th occupied pair.
// A snapshot index is used rather than a cursor so nested loops over the
// same dictionary each get an independent traversal.
int64_t suki_dict_entry_count(const SukiDict* d) {
    return suki_dict_len(d);
}

int32_t suki_dict_entry_at(const SukiDict* d, int64_t index,
                           void* out_key, void* out_value) {
    if (!d || !d->data || index < 0) return 0;
    SukiDictView v = suki_dict_view(d);
    int64_t cap = d->capacity;
    int64_t seen = 0;
    for (int64_t i = 0; i < cap; ++i) {
        unsigned char* slot = suki_dict_slot(&v, i);
        // The occupancy byte lives right after the key, not at slot[0]: a key's
        // first byte carries real data and cannot be used to tell a live entry
        // from an empty one (or from a tombstone left behind by a removal).
        if (slot[v.key_size] != SUKI_DICT_FULL) continue;
        if (seen == index) {
            if (out_key && v.key_size > 0)
                memcpy(out_key, slot, (size_t)v.key_size);
            if (out_value && v.val_size > 0)
                memcpy(out_value, slot + v.key_size + 1, (size_t)v.val_size);
            return 1;
        }
        ++seen;
    }
    return 0;
}

void suki_dict_free(SukiDict d) { if (d.data) suki_free(d.data); }

// ─── weak references ───────────────────────────────────────────────────────
// Side table linking every registered weak slot to its object. A single global
// lock guards the list; registration is rare compared to loading, so loading
// stays lock-free (an acquire load of the slot).
typedef struct SukiWeakEntry {
    void* object;
    void** slot;
    struct SukiWeakEntry* next;
} SukiWeakEntry;

static SukiWeakEntry* suki_weak_list = NULL;
static int suki_weak_lock = 0;

// Acquire/release spin lock guarding the weak side table.
static void suki_weak_lock_acquire(void) {
    while (__atomic_test_and_set(&suki_weak_lock, __ATOMIC_ACQUIRE)) { }
}
static void suki_weak_lock_release(void) {
    __atomic_clear(&suki_weak_lock, __ATOMIC_RELEASE);
}

void suki_weak_register(void* object, void** slot) {
    if (!object || !slot) return;
    SukiWeakEntry* e = (SukiWeakEntry*)suki_alloc(sizeof(SukiWeakEntry));
    if (!e) return;
    suki_weak_lock_acquire();
    e->object = object;
    e->slot = slot;
    e->next = suki_weak_list;
    suki_weak_list = e;
    suki_weak_lock_release();
}

void suki_weak_unregister(void* const* slot) {
    if (!slot) return;
    suki_weak_lock_acquire();
    SukiWeakEntry** link = &suki_weak_list;
    while (*link) {
        if ((*link)->slot == (void**)slot) {
            SukiWeakEntry* dead = *link;
            *link = dead->next;
            suki_free(dead);
            break;
        }
        link = &(*link)->next;
    }
    suki_weak_lock_release();
}

void* suki_weak_load(void* const* slot) {
    // Acquire pairs with the release in suki_weak_clear_all, so seeing a NULL
    // here also guarantees the writes that preceded the clearing are visible.
    return slot ? __atomic_load_n(slot, __ATOMIC_ACQUIRE) : NULL;
}

void suki_weak_clear_all(void* object) {
    suki_weak_lock_acquire();
    SukiWeakEntry** link = &suki_weak_list;
    while (*link) {
        SukiWeakEntry* e = *link;
        if (e->object == object) {
            // Release: a reader that observes NULL must not reorder earlier
            // writes past this point.
            __atomic_store_n(e->slot, (void*)NULL, __ATOMIC_RELEASE);
            *link = e->next;
            suki_free(e);
        } else {
            link = &e->next;
        }
    }
    suki_weak_lock_release();
}

// ─── synchronisation ───────────────────────────────────────────────────────
void suki_mutex_init(SukiMutex* m) {
    if (m) __atomic_store_n(&m->state, 0, __ATOMIC_RELAXED);
}

void suki_mutex_lock(SukiMutex* m) {
    if (!m) return;
    // Test-and-set with acquire semantics: pairs with the release in unlock.
    while (__atomic_test_and_set(&m->state, __ATOMIC_ACQUIRE)) { }
}

void suki_mutex_unlock(SukiMutex* m) {
    if (!m) return;
    __atomic_clear(&m->state, __ATOMIC_RELEASE);
}

void suki_mutex_destroy(SukiMutex* m) { (void)m; }

// ─── raw pointer helpers ───────────────────────────────────────────────────────
// Exposed to @unsafe contexts through the `memory` standard-library module. The
// address is passed as an `Int` by the generated code (LP64 makes an 8-byte
// pointer and an int64 the same width), so no opaque-pointer type is needed on
// the C side. The helpers simply reinterpret the bits as a pointer.
void* suki_ptr_alloc(int64_t n) {
    return suki_alloc((size_t)(n > 0 ? n : 1));
}

int64_t suki_ptr_drop(void* p) { suki_free(p); return 0; }

int64_t suki_ptr_read_int(void* p) {
    if (!p) return 0;
    return *(int64_t*)p;
}

int64_t suki_ptr_write_int(void* p, int64_t v) {
    if (!p) return 0;
    *(int64_t*)p = v;
    return 0;
}

int64_t suki_atomic_load_i64(const int64_t* p) {
    return p ? __atomic_load_n(p, __ATOMIC_ACQUIRE) : 0;
}

void suki_atomic_store_i64(int64_t* p, int64_t v) {
    if (p) __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

int64_t suki_atomic_add_i64(int64_t* p, int64_t delta) {
    return p ? __atomic_fetch_add(p, delta, __ATOMIC_ACQ_REL) + delta : 0;
}

// ─── portable threading ─────────────────────────────────────────────────────────
#if defined(_WIN32)
typedef CRITICAL_SECTION suki_bmtx_t;
typedef CONDITION_VARIABLE suki_cvar_t;
#else
typedef pthread_mutex_t suki_bmtx_t;
typedef pthread_cond_t  suki_cvar_t;
#endif

static void suki_bmtx_init(suki_bmtx_t* m) {
#if defined(_WIN32)
    InitializeCriticalSection(m);
#else
    pthread_mutex_init(m, NULL);
#endif
}
static void suki_bmtx_lock(suki_bmtx_t* m) {
#if defined(_WIN32)
    EnterCriticalSection(m);
#else
    pthread_mutex_lock(m);
#endif
}
static void suki_bmtx_unlock(suki_bmtx_t* m) {
#if defined(_WIN32)
    LeaveCriticalSection(m);
#else
    pthread_mutex_unlock(m);
#endif
}
static void suki_bmtx_destroy(suki_bmtx_t* m) {
#if defined(_WIN32)
    DeleteCriticalSection(m);
#else
    pthread_mutex_destroy(m);
#endif
}
static void suki_cvar_init(suki_cvar_t* c) {
#if defined(_WIN32)
    InitializeConditionVariable(c);
#else
    pthread_cond_init(c, NULL);
#endif
}
// Must be called with `m` held. Releases `m` while waiting and re-acquires it
// before returning, exactly like pthread_cond_wait.
static void suki_cvar_wait(suki_cvar_t* c, suki_bmtx_t* m) {
#if defined(_WIN32)
    SleepConditionVariableCS(c, m, INFINITE);
#else
    pthread_cond_wait(c, m);
#endif
}
static void suki_cvar_broadcast(suki_cvar_t* c) {
#if defined(_WIN32)
    WakeAllConditionVariable(c);
#else
    pthread_cond_broadcast(c);
#endif
}
static void suki_cvar_destroy(suki_cvar_t* c) {
#if defined(_WIN32)
    (void)c; // Win32 condition variables need no teardown
#else
    pthread_cond_destroy(c);
#endif
}

void suki_sleep(int64_t ms) {
    if (ms <= 0) return;
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000);
    req.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&req, NULL);
#endif
}

// A CAS loop on the first word of an `Int` makes a usable spin lock for the
// concurrency stress scenarios the language binding exercises.
void suki_spin_lock(int64_t* lock) {
    int64_t expected = 0;
    while (!__atomic_compare_exchange_n(lock, &expected, 1, 1,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        expected = 0;
    }
}

void suki_spin_unlock(int64_t* lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

// ─── actor 串行执行器（规范 §7.4）────────────────────────────────────────────
// actor 的隔离语义不只是"任意时刻只有一个线程在跑"：规范要求一个**串行执行器
// 队列**，并且 actor 方法在 `await` 挂起期间必须**释放**隔离，让其他任务得以
// 进入（重入 / reentrancy），恢复后再重新排队取回执行权。
//
// 实现上把 actor 的锁字（对象头 offset 3，一个 i64）打包成 FIFO 票号锁：
//   低 32 位 = serving：当前正在被服务的票号
//   高 32 位 = next   ：下一个将要发出的票号
// enter 取票（next++）后自旋等待 serving 追上自己的票号，因此先到先服务、
// 不会像普通自旋锁那样饥饿或乱序 —— 这正是串行执行器队列的语义。
// leave 推进 serving，把执行权交给队列中的下一个任务。
//
// 重入由 codegen 完成：actor 方法体内的每个 `await` 挂起点前后分别插入
// suki_actor_leave / suki_actor_enter，从而挂起期间放行其他任务。
void suki_actor_enter(int64_t* lock) {
    if (!lock) return;
    // 取票：next 自增，自增前的 next 即自己的票号。
    uint64_t prev =
        (uint64_t)__atomic_fetch_add(lock, (int64_t)1 << 32, __ATOMIC_ACQ_REL);
    uint32_t my = (uint32_t)(prev >> 32);
    for (;;) {
        uint64_t cur = (uint64_t)__atomic_load_n(lock, __ATOMIC_ACQUIRE);
        if ((uint32_t)(cur & 0xffffffffu) == my) return; // 轮到自己
        // 可移植的自旋提示：不调用任何让出/睡眠函数（它们在部分构建配置下
        // 不可用），仅以内存屏障打断忙等；actor 临界区通常很短，开销可接受。
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
}

void suki_actor_leave(int64_t* lock) {
    if (!lock) return;
    // 推进 serving：把执行权交给队列中的下一个任务。
    __atomic_fetch_add(lock, 1, __ATOMIC_ACQ_REL);
}

// actor 实例初始化时清零锁字（serving=0, next=0）。
void suki_actor_init(int64_t* lock) {
    if (lock) __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

// Threads are created detached. A Future (not a join) is how completion is
// observed, which keeps the code identical on every platform and avoids leaking
// joinable threads when a result is never awaited.
#if defined(_WIN32)
typedef struct { SukiThreadFn fn; void* arg; } SukiThreadStart;

static unsigned __stdcall suki_win_thread_proc(void* p) {
    SukiThreadStart* ts = (SukiThreadStart*)p;
    ts->fn(ts->arg);
    suki_free(ts);
    return 0;
}
#endif

int suki_thread_start(SukiThreadFn fn, void* arg) {
#if defined(_WIN32)
    SukiThreadStart* ts = (SukiThreadStart*)suki_alloc(sizeof(SukiThreadStart));
    if (!ts) return -1;
    ts->fn = fn;
    ts->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, suki_win_thread_proc, ts, 0, NULL);
    if (!h) { suki_free(ts); return -1; }
    // Closing the handle detaches the thread; it cleans up when it returns.
    CloseHandle((HANDLE)h);
    return 0;
#else
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0) return -1;
    pthread_detach(t);
    return 0;
#endif
}

// ─── Future ─────────────────────────────────────────────────────────────────────
struct SukiFuture {
    int64_t      done;
    void*        result;      // resultSize bytes, owned by the Future
    int64_t      resultSize;
    suki_bmtx_t  mtx;
    suki_cvar_t  cvar;
};

SukiFuture* suki_future_create(int64_t resultSize) {
    SukiFuture* f = (SukiFuture*)suki_alloc(sizeof(SukiFuture));
    f->done = 0;
    f->resultSize = resultSize > 0 ? resultSize : 0;
    f->result = f->resultSize > 0 ? suki_alloc((size_t)f->resultSize) : NULL;
    suki_bmtx_init(&f->mtx);
    suki_cvar_init(&f->cvar);
    return f;
}

void suki_future_store(SukiFuture* f, const void* src) {
    if (!f || !f->result || !src || f->resultSize <= 0) return;
    memcpy(f->result, src, (size_t)f->resultSize);
}

void suki_future_finish(SukiFuture* f) {
    if (!f) return;
    suki_bmtx_lock(&f->mtx);
    f->done = 1;
    suki_cvar_broadcast(&f->cvar);
    suki_bmtx_unlock(&f->mtx);
}

void suki_future_await(SukiFuture* f, void* out) {
    if (!f || !out) return;
    suki_bmtx_lock(&f->mtx);
    while (!f->done) suki_cvar_wait(&f->cvar, &f->mtx);
    if (f->result && f->resultSize > 0)
        memcpy(out, f->result, (size_t)f->resultSize);
    suki_bmtx_unlock(&f->mtx);
}

void suki_future_free(SukiFuture* f) {
    if (!f) return;
    if (f->result) suki_free(f->result);
    suki_cvar_destroy(&f->cvar);
    suki_bmtx_destroy(&f->mtx);
    suki_free(f);
}

// ─── Channel ────────────────────────────────────────────────────────────────────
struct SukiChannel {
    suki_bmtx_t    mtx;
    suki_cvar_t    notFull;
    suki_cvar_t    notEmpty;
    int64_t        capacity;   // number of element slots
    int64_t        elemSize;   // bytes per element
    int64_t        count;      // elements currently buffered
    int64_t        head;       // index of the oldest element
    int64_t        closed;
    unsigned char* buf;
};

SukiChannel* suki_channel_create(int64_t capacity, int64_t elemSize) {
    if (capacity < 1) capacity = 1;
    if (elemSize < 1) elemSize = 1;
    SukiChannel* c = (SukiChannel*)suki_alloc(sizeof(SukiChannel));
    c->capacity = capacity;
    c->elemSize = elemSize;
    c->count = 0;
    c->head = 0;
    c->closed = 0;
    size_t bufBytes;
    if (suki_size_mul(capacity, elemSize, &bufBytes)) panic("channel size overflow");
    c->buf = (unsigned char*)suki_alloc(bufBytes ? bufBytes : 1);
    suki_bmtx_init(&c->mtx);
    suki_cvar_init(&c->notFull);
    suki_cvar_init(&c->notEmpty);
    return c;
}

void suki_channel_send(SukiChannel* c, const void* elem) {
    if (!c || !elem) return;
    suki_bmtx_lock(&c->mtx);
    while (!c->closed && c->count == c->capacity)
        suki_cvar_wait(&c->notFull, &c->mtx);
    if (!c->closed) {
        size_t off = (size_t)((c->head + c->count) % c->capacity) * (size_t)c->elemSize;
        memcpy(c->buf + off, elem, (size_t)c->elemSize);
        c->count += 1;
        suki_cvar_broadcast(&c->notEmpty);
    }
    suki_bmtx_unlock(&c->mtx);
}

int32_t suki_channel_receive(SukiChannel* c, void* out_elem) {
    if (!c || !out_elem) return -1;
    suki_bmtx_lock(&c->mtx);
    while (c->count == 0 && !c->closed)
        suki_cvar_wait(&c->notEmpty, &c->mtx);
    if (c->count == 0) {          // closed and drained
        suki_bmtx_unlock(&c->mtx);
        return -1;
    }
    size_t off = (size_t)c->head * (size_t)c->elemSize;
    memcpy(out_elem, c->buf + off, (size_t)c->elemSize);
    c->head = (c->head + 1) % c->capacity;
    c->count -= 1;
    suki_cvar_broadcast(&c->notFull);
    suki_bmtx_unlock(&c->mtx);
    return 0;
}

int32_t suki_channel_try_send(SukiChannel* c, const void* elem) {
    if (!c || !elem) return -1;
    suki_bmtx_lock(&c->mtx);
    if (c->closed) { suki_bmtx_unlock(&c->mtx); return -1; }
    if (c->count == c->capacity) { suki_bmtx_unlock(&c->mtx); return 0; }
    size_t off = (size_t)((c->head + c->count) % c->capacity) * (size_t)c->elemSize;
    memcpy(c->buf + off, elem, (size_t)c->elemSize);
    c->count += 1;
    suki_cvar_broadcast(&c->notEmpty);
    suki_bmtx_unlock(&c->mtx);
    return 1;
}

int32_t suki_channel_try_receive(SukiChannel* c, void* out_elem) {
    if (!c || !out_elem) return -1;
    suki_bmtx_lock(&c->mtx);
    if (c->count == 0) { suki_bmtx_unlock(&c->mtx); return c->closed ? -1 : 0; }
    size_t off = (size_t)c->head * (size_t)c->elemSize;
    memcpy(out_elem, c->buf + off, (size_t)c->elemSize);
    c->head = (c->head + 1) % c->capacity;
    c->count -= 1;
    suki_cvar_broadcast(&c->notFull);
    suki_bmtx_unlock(&c->mtx);
    return 1;
}

void suki_channel_close(SukiChannel* c) {
    if (!c) return;
    suki_bmtx_lock(&c->mtx);
    c->closed = 1;
    suki_cvar_broadcast(&c->notEmpty);
    suki_cvar_broadcast(&c->notFull);
    suki_bmtx_unlock(&c->mtx);
}

void suki_channel_free(SukiChannel* c) {
    if (!c) return;
    if (c->buf) suki_free(c->buf);
    suki_cvar_destroy(&c->notEmpty);
    suki_cvar_destroy(&c->notFull);
    suki_bmtx_destroy(&c->mtx);
    suki_free(c);
}

// ─── Task / TaskGroup ───────────────────────────────────────────────────────────
typedef struct { SukiClosureFn fn; void* ctx; SukiFuture* fut; } SukiClosureStart;

#if defined(_WIN32)
static unsigned __stdcall suki_closure_thread_proc(void* p) {
    SukiClosureStart* s = (SukiClosureStart*)p;
    s->fn(s->ctx);
    suki_future_finish(s->fut);
    suki_free(s);
    return 0;
}
#else
static void* suki_closure_thread_proc(void* p) {
    SukiClosureStart* s = (SukiClosureStart*)p;
    s->fn(s->ctx);
    suki_future_finish(s->fut);
    suki_free(s);
    return NULL;
}
#endif

int suki_closure_thread_start(SukiClosureFn fn, void* ctx, SukiFuture* fut) {
    if (!fn) return -1;
    SukiClosureStart* s = (SukiClosureStart*)suki_alloc(sizeof(SukiClosureStart));
    if (!s) return -1;
    s->fn = fn;
    s->ctx = ctx;
    s->fut = fut;
#if defined(_WIN32)
    uintptr_t h = _beginthreadex(NULL, 0, suki_closure_thread_proc, s, 0, NULL);
    if (!h) { suki_free(s); return -1; }
    CloseHandle((HANDLE)h);
    return 0;
#else
    pthread_t t;
    if (pthread_create(&t, NULL, suki_closure_thread_proc, s) != 0) {
        suki_free(s);
        return -1;
    }
    pthread_detach(t);
    return 0;
#endif
}

struct SukiTaskGroup {
    suki_bmtx_t   mtx;
    SukiFuture**  futures;
    int64_t       count;
    int64_t       capacity;
    int64_t       elemSize;   // byte size of a child's result, 0 for none
};

SukiTaskGroup* suki_taskgroup_create(int64_t elemSize) {
    SukiTaskGroup* g = (SukiTaskGroup*)suki_alloc(sizeof(SukiTaskGroup));
    g->capacity = 8;
    g->count = 0;
    g->elemSize = elemSize > 0 ? elemSize : 0;
    g->futures = (SukiFuture**)suki_alloc(sizeof(SukiFuture*) * (size_t)g->capacity);
    suki_bmtx_init(&g->mtx);
    return g;
}

int64_t suki_taskgroup_count(SukiTaskGroup* g) {
    if (!g) return 0;
    suki_bmtx_lock(&g->mtx);
    int64_t n = g->count;
    suki_bmtx_unlock(&g->mtx);
    return n;
}

int32_t suki_taskgroup_result_at(SukiTaskGroup* g, int64_t index,
                                 void* out_elem) {
    if (!g || !out_elem || index < 0) return -1;
    suki_bmtx_lock(&g->mtx);
    if (index >= g->count) { suki_bmtx_unlock(&g->mtx); return -1; }
    SukiFuture* f = g->futures[index];
    suki_bmtx_unlock(&g->mtx);
    if (!f) return -1;
    // Await outside the lock: the child signals completion without needing it.
    // 写回与否依据 future 自身的大小（startClosureTask 已按结果类型大小分配），
    // 而非 group 的 elemSize——后者在 withTaskGroup 创建时固定为 0，会导致结果
    // 被丢弃而留下未初始化垃圾值。
    int64_t dummy = 0;
    if (f->resultSize > 0) suki_future_await(f, out_elem);
    else suki_future_await(f, &dummy);
    return f->resultSize > 0 ? 0 : -1;
}

void suki_taskgroup_add(SukiTaskGroup* g, SukiFuture* f) {
    if (!g || !f) return;
    suki_bmtx_lock(&g->mtx);
    if (g->count == g->capacity) {
        int64_t nc = g->capacity * 2;
        if (nc <= g->capacity) panic("task group capacity overflow");
        size_t nfBytes;
        if (suki_size_mul((int64_t)sizeof(SukiFuture*), nc, &nfBytes))
            panic("task group size overflow");
        SukiFuture** nf = (SukiFuture**)suki_alloc(nfBytes);
        for (int64_t i = 0; i < g->count; ++i) nf[i] = g->futures[i];
        suki_free(g->futures);
        g->futures = nf;
        g->capacity = nc;
    }
    g->futures[g->count++] = f;
    suki_bmtx_unlock(&g->mtx);
}

void suki_taskgroup_wait_all(SukiTaskGroup* g) {
    if (!g) return;
    // Snapshot under the lock, then wait outside it: a child that finishes may
    // still be touching the group, and holding the lock while waiting would
    // deadlock against `add`.
    suki_bmtx_lock(&g->mtx);
    int64_t n = g->count;
    SukiFuture** snap = NULL;
    if (n > 0) {
        snap = (SukiFuture**)suki_alloc(sizeof(SukiFuture*) * (size_t)n);
        for (int64_t i = 0; i < n; ++i) snap[i] = g->futures[i];
    }
    suki_bmtx_unlock(&g->mtx);
    if (!snap) return;
    int64_t dummy = 0;
    for (int64_t i = 0; i < n; ++i) suki_future_await(snap[i], &dummy);
    suki_free(snap);
}

void suki_taskgroup_free(SukiTaskGroup* g) {
    if (!g) return;
    if (g->futures) {
        for (int64_t i = 0; i < g->count; ++i)
            if (g->futures[i]) suki_future_free(g->futures[i]);
        suki_free(g->futures);
    }
    suki_bmtx_destroy(&g->mtx);
    suki_free(g);
}

// ─── C 互操作测试桩（规范 §6.3 / @convention）──────────────────────────────
// 供 SukiCode 侧以 `@_cdecl("suki_test_add_c") @convention(c) extern` 声明并
// 调用，验证 C 调用约定路径端到端可用。C 默认调用约定即 CallingConv::C，与
// 声明一致，链接后可直接调用。
int64_t suki_test_add_c(int64_t a, int64_t b) {
    return a + b;
}

// 供 @convention(stdcall) 端到端测试使用独立符号（与 C 约定变体同名会导致
// 链接器产生 `.1` 后缀冲突）。x86_64 上 stdcall 按默认 ABI 处理，链接后可直接调用。
int64_t suki_test_add_c_std(int64_t a, int64_t b) {
    return a + b;
}
