#pragma once

// Public interface of the SukiCode C runtime. Generated code and the
// bootstrap standard library call these symbols directly.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void print(const char* s);
void println(const char* s);
void panic(const char* msg);

void* suki_alloc(size_t size);
void  suki_free(void* p);

uint64_t suki_arc_retain(void* obj);
uint64_t suki_arc_release(void* obj);

// ─── String ABI ────────────────────────────────────────────────────────────
// SukiCode `String` lowers to the identical LLVM aggregate:
//   %SukiString = type { i8* data, i64 length }
// so these entry points take it by value exactly as the IR declares them.
// `data` is always NUL-terminated at [length] for C interoperability.
typedef struct {
    const char* data;
    int64_t length;
} SukiString;

SukiString suki_str_lit(const char* utf8, int64_t len);
int64_t    suki_str_length(SukiString s);
const char* suki_str_data(SukiString s);
SukiString suki_str_concat(SukiString a, SukiString b);
SukiString suki_str_from_cstr(const char* z);
int32_t    suki_str_compare(SukiString a, SukiString b);
int32_t    suki_str_has_prefix(SukiString s, SukiString prefix);
int32_t    suki_str_has_suffix(SukiString s, SukiString suffix);
int32_t    suki_str_utf8_count(SukiString s);
int32_t    suki_str_utf8_get(SukiString s, int32_t index);

SukiString suki_int_to_string(int64_t v);
// Renders a Unicode scalar (Char) as a one-character String.
SukiString suki_char_to_string(int32_t v);
// Parses a String into a number (`Int("42")`). Leading/trailing spaces are
// ignored; a non-numeric string yields 0. Double accepts a fraction/exponent.
int64_t suki_str_to_int(SukiString s);
double  suki_str_to_double(SukiString s);
SukiString suki_double_to_string(double v);

void suki_print_str(SukiString s);
void suki_println_str(SukiString s);

// ─── Array ABI ─────────────────────────────────────────────────────────────
// `Array<T>` lowers to `{ T* data, i64 length, i64 capacity }`. The element
// size is stored in a hidden header word immediately before `data` so that
// growth and indexing work for any element type without a thunk.
typedef struct {
    void* data;
    int64_t length;
    int64_t capacity;
} SukiArray;

// Array mutators take the array by pointer: passing/returning the aggregate by
// value would depend on the hidden-struct-return (sret) convention, which is not
// guaranteed to agree between the clang that builds the IR and the compiler that
// builds this runtime. An explicit out-pointer is ABI-stable everywhere.
void     suki_array_new(int64_t elem_size, int64_t capacity, SukiArray* out);
int64_t  suki_array_len(const SukiArray* a);
void     suki_array_push(SukiArray* a, int64_t elem_size, const void* value);
void*    suki_array_get(const SukiArray* a, int64_t index);
void     suki_array_set(const SukiArray* a, int64_t index, const void* value);
void     suki_array_remove_at(SukiArray* a, int64_t index);
void     suki_array_free(SukiArray a);

// ─── Dictionary / Set ABI ──────────────────────────────────────────────────
// `Dictionary<K,V>` and `Set<T>` share one open-addressing hash table. The
// aggregate is `{ void* data, int64 count, i64 capacity }`.
//
// The block behind `data` is laid out as:
//     [ int64 key_size | int64 val_size | capacity * slot ]
// where each slot is `key_size` key bytes, one occupancy byte, then
// `val_size` value bytes. `Set<T>` is a dictionary with val_size == 0, which
// is why both share every entry point below.
//
// Keys are compared bytewise and hashed with FNV-1a, so any fixed-size key
// type works without the runtime knowing anything about it.
typedef struct {
    void* data;
    int64_t count;
    int64_t capacity;
} SukiDict;

void    suki_dict_new(int64_t key_size, int64_t val_size, int64_t capacity,
                      SukiDict* out);
int64_t suki_dict_len(const SukiDict* d);
// Returns 1 when the key was found; its value is copied into `out_value`.
int32_t suki_dict_get(const SukiDict* d, int64_t key_size, int64_t val_size,
                      const void* key, void* out_value);
void    suki_dict_set(SukiDict* d, int64_t key_size, int64_t val_size,
                      const void* key, const void* value);
int32_t suki_dict_remove(SukiDict* d, int64_t key_size, int64_t val_size,
                         const void* key);
void    suki_dict_free(SukiDict d);

// Iteration support: `suki_dict_entry_count` reports how many slots are
// occupied, and `suki_dict_entry_at` copies the i-th occupied key/value pair
// into caller-provided buffers. Keys and values are compared bytewise, so the
// caller supplies the element sizes it used when creating the table.
int64_t suki_dict_entry_count(const SukiDict* d);
int32_t suki_dict_entry_at(const SukiDict* d, int64_t index,
                           void* out_key, void* out_value);

// ─── weak references ───────────────────────────────────────────────────────
// A `weak` variable holds a *slot* (an `i8*` cell) rather than the object
// itself. Registering records the (object, slot) pair in a side table so that
// when the object's last strong reference goes away, every slot referring to
// it is set to NULL atomically — this is what breaks retain cycles.
void  suki_weak_register(void* object, void** slot);
void  suki_weak_unregister(void* const* slot);
void* suki_weak_load(void* const* slot);
// Invoked from deinit: clears every slot registered for `object`.
void  suki_weak_clear_all(void* object);

// ─── synchronisation primitives ────────────────────────────────────────────
// A spin lock built on an atomic flag. Acquire/release ordering (never
// relaxed) pairs with the ARC counters, so writes made while holding the lock
// are visible to the next holder.
typedef struct { int64_t state; } SukiMutex;

void suki_mutex_init(SukiMutex* m);
void suki_mutex_lock(SukiMutex* m);
void suki_mutex_unlock(SukiMutex* m);
void suki_mutex_destroy(SukiMutex* m);

// Atomic scalar helpers, used by generated code for atomic access.
int64_t suki_atomic_load_i64(const int64_t* p);
void    suki_atomic_store_i64(int64_t* p, int64_t v);
int64_t suki_atomic_add_i64(int64_t* p, int64_t delta);

#ifdef __cplusplus
}
#endif
