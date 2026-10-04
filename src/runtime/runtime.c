// SukiCode minimal C runtime.
//
// Provides the primitives the compiler-generated code and the bootstrap
// standard library rely on: printing, panic, and aligned allocation.
// The object-code stage links this runtime with the user's translation unit.
//
// ARC reference counting (suki_arc_retain/release) uses acquire/release
// memory ordering; only debug counters use relaxed ordering.

#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        // Last strong reference: run `deinit` before releasing the storage. The
        // slot is null for classes that declare none; a subclass that does not
        // declare its own simply inherits the pointer the constructor stored.
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

int64_t suki_str_length(SukiString s) { return s.length; }

const char* suki_str_data(SukiString s) { return s.data ? s.data : ""; }

SukiString suki_str_concat(SukiString a, SukiString b) {
    int64_t n = a.length + b.length;
    char* buf = (char*)suki_alloc((size_t)n + 1);
    if (a.length) memcpy(buf, a.data, (size_t)a.length);
    if (b.length) memcpy(buf + a.length, b.data, (size_t)b.length);
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
    return memcmp(s.data, prefix.data, (size_t)prefix.length) == 0;
}

int32_t suki_str_has_suffix(SukiString s, SukiString suffix) {
    if (suffix.length > s.length) return 0;
    if (suffix.length == 0) return 1;
    return memcmp(s.data + (s.length - suffix.length), suffix.data,
                  (size_t)suffix.length) == 0;
}

int32_t suki_str_utf8_count(SukiString s) {    int32_t n = 0;
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
    int64_t i = 0;
    int32_t n = 0;
    while (i < s.length) {
        const unsigned char* p = (const unsigned char*)s.data + i;
        int32_t w = suki_utf8_seq_len(p);
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
    memcpy(buf, s.data, (size_t)n);
    buf[n] = '\0';
    return (int64_t)strtoll(buf, NULL, 10);
}

double suki_str_to_double(SukiString s) {
    char buf[64];
    int64_t n = s.length < 63 ? s.length : 63;
    if (n < 0) n = 0;
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

// ─── Array ────────────────────────────────────────────────────────────────
// Storage is a single block: [ int64 elem_size | capacity * elem_size bytes ].
// `data` points just past the header, so element i is `data[i]` for any type.
void suki_array_new(int64_t elem_size, int64_t capacity, SukiArray* out) {
    if (!out) return;
    if (elem_size <= 0) elem_size = 1;
    if (capacity < 0) capacity = 0;
    int64_t bytes = capacity * elem_size;
    char* block = (char*)suki_alloc((size_t)(sizeof(int64_t) + (bytes ? bytes : 1)));
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
        char* newBlock = (char*)suki_alloc((size_t)(sizeof(int64_t) +
                                                   newCap * elem_size));
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
    int64_t slot = key_size + 1 + val_size;
    int64_t total = SUKI_DICT_HDR + capacity * slot;
    char* block = (char*)suki_alloc((size_t)total);
    *(int64_t*)block = key_size;
    *(int64_t*)(block + 8) = val_size;
    memset(block + SUKI_DICT_HDR, SUKI_DICT_EMPTY, (size_t)(capacity * slot));
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
    SukiDict fresh;
    suki_dict_new(old.key_size, old.val_size, oldCap * 2, &fresh);
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
    if (!s) return;
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

int64_t suki_atomic_load_i64(const int64_t* p) {
    return p ? __atomic_load_n(p, __ATOMIC_ACQUIRE) : 0;
}

void suki_atomic_store_i64(int64_t* p, int64_t v) {
    if (p) __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

int64_t suki_atomic_add_i64(int64_t* p, int64_t delta) {
    return p ? __atomic_fetch_add(p, delta, __ATOMIC_ACQ_REL) + delta : 0;
}
