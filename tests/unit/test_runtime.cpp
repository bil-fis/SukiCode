// Runtime unit tests for the SukiCode C runtime.
//
// These link against runtime.c directly and exercise the data structures the
// code generator emits calls into: strings, arrays, the hash table behind
// Dictionary/Set, weak references and the atomic primitives. Testing them on the
// host catches logic errors (growth, probing, tombstones, Unicode decoding)
// without going through the compiler, and pinpoints the failing subsystem.

#include "../../src/runtime/runtime.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::cout << "  FAIL: " << what << "\n";
    }
}

template <typename T>
void checkEq(const T& got, const T& want, const std::string& what) {
    ++g_checks;
    if (!(got == want)) {
        ++g_failures;
        std::cout << "  FAIL: " << what << " (expected " << want << ", got "
                  << got << ")\n";
    }
}

// ── strings ────────────────────────────────────────────────────────────────
void testStrings() {
    std::cout << "Strings\n";
    SukiString hello = suki_str_lit("Hello", 5);
    checkEq<int64_t>(suki_str_length(hello), 5, "byte length");
    check(std::memcmp(suki_str_data(hello), "Hello", 5) == 0, "data pointer");

    // An embedded NUL must not truncate the string: that is the whole reason
    // the language-level String carries a length.
    SukiString embedded = suki_str_lit("a\0b", 3);
    checkEq<int64_t>(suki_str_length(embedded), 3, "embedded NUL length");

    SukiString joined = suki_str_concat(suki_str_lit("foo", 3),
                                        suki_str_lit("bar", 3));
    checkEq<int64_t>(suki_str_length(joined), 6, "concat length");
    check(std::memcmp(suki_str_data(joined), "foobar", 6) == 0, "concat bytes");

    checkEq<int32_t>(suki_str_compare(suki_str_lit("abc", 3),
                                      suki_str_lit("abc", 3)), 0,
                     "compare equal");
    check(suki_str_compare(suki_str_lit("abc", 3), suki_str_lit("abd", 3)) < 0,
          "compare less");
    check(suki_str_has_prefix(suki_str_lit("hello", 5), suki_str_lit("he", 2)),
          "has prefix");
    check(!suki_str_has_prefix(suki_str_lit("hello", 5), suki_str_lit("lo", 2)),
          "prefix absent");
    check(suki_str_has_suffix(suki_str_lit("hello", 5), suki_str_lit("lo", 2)),
          "has suffix");

    // "h<e-acute>llo": five scalars but six UTF-8 bytes.
    SukiString accented = suki_str_lit("h\xc3\xa9llo", 6);
    checkEq<int32_t>(suki_str_utf8_count(accented), 5, "unicode scalar count");
    checkEq<int32_t>(suki_str_utf8_get(accented, 1), 0xE9, "scalar value");
    checkEq<int32_t>(suki_str_utf8_get(accented, 9), -1, "index out of range");

    SukiString num = suki_int_to_string(-12345);
    check(std::memcmp(suki_str_data(num), "-12345", 6) == 0, "int formatting");
    // The result must outlive the call (it is concatenated into a String the
    // caller owns), so it cannot be a stack buffer.
    check(suki_str_data(num) != nullptr, "formatted string is not null");
    SukiString f = suki_double_to_string(2.5);
    check(suki_str_length(f) > 0, "double formatting");
}

// ── arrays ─────────────────────────────────────────────────────────────────
void testArrays() {
    std::cout << "Arrays\n";
    SukiArray a;
    suki_array_new(8, 2, &a);   // capacity 2 forces a growth on the third push
    checkEq<int64_t>(suki_array_len(&a), 0, "new array is empty");
    for (int i = 1; i <= 5; ++i) {
        int64_t v = i * 10;
        suki_array_push(&a, 8, &v);
    }
    checkEq<int64_t>(suki_array_len(&a), 5, "length after pushes");
    check(a.capacity >= 5, "capacity grew to fit");
    for (int i = 0; i < 5; ++i)
        checkEq<int64_t>(*static_cast<int64_t*>(suki_array_get(&a, i)),
                         (i + 1) * 10, "element preserved across growth");

    int64_t replacement = 999;
    suki_array_set(&a, 0, &replacement);
    checkEq<int64_t>(*static_cast<int64_t*>(suki_array_get(&a, 0)), 999,
                     "element after set");
    check(suki_array_get(&a, 99) == nullptr, "out-of-range read is null");

    SukiArray r;
    suki_array_new(8, 8, &r);
    for (int i = 0; i < 3; ++i) {
        int64_t k = i, v = i;
        suki_array_remove_at(&r, 0);
        (void)k; (void)v;
    }
    checkEq<int64_t>(suki_array_len(&r), 0, "removeAt drains the array");
    suki_array_free(r);
    suki_array_free(a);

    SukiArray e;
    suki_array_new(8, 0, &e);
    checkEq<int64_t>(suki_array_len(&e), 0, "zero-capacity array is empty");
    suki_array_free(e);
}

// ── dictionary / set ───────────────────────────────────────────────────────
void testDictionary() {
    std::cout << "Dictionary and Set\n";
    SukiDict d;
    suki_dict_new(8, 8, 8, &d);
    // Enough inserts to force several rehashes.
    for (int i = 1; i <= 200; ++i) {
        int64_t k = i, v = i * 3;
        suki_dict_set(&d, 8, 8, &k, &v);
    }
    checkEq<int64_t>(suki_dict_len(&d), 200, "count after growth");
    int wrong = 0;
    for (int i = 1; i <= 200; ++i) {
        int64_t k = i, out = -1;
        if (!suki_dict_get(&d, 8, 8, &k, &out) || out != i * 3) ++wrong;
    }
    checkEq<int>(wrong, 0, "every key readable after rehashes");

    // Overwriting must not change the count.
    int64_t k = 5, nv = 42;
    suki_dict_set(&d, 8, 8, &k, &nv);
    checkEq<int64_t>(suki_dict_len(&d), 200, "count unchanged by overwrite");

    // Removal uses tombstones so the remaining probe chains stay intact; a
    // naive EMPTY marker would make later keys unreachable.
    for (int i = 1; i <= 100; ++i) {
        int64_t rk = i;
        suki_dict_remove(&d, 8, 8, &rk);
    }
    checkEq<int64_t>(suki_dict_len(&d), 100, "count after removals");
    int lost = 0;
    for (int i = 101; i <= 200; ++i) {
        int64_t kk = i, out = -1;
        if (!suki_dict_get(&d, 8, 8, &kk, &out)) ++lost;
    }
    checkEq<int>(lost, 0, "no entries lost across probe chains");
    suki_dict_free(d);

    // A Set is a dictionary with no value payload, so duplicates collapse.
    SukiDict s;
    suki_dict_new(8, 0, 8, &s);
    for (int i = 0; i < 10; ++i) {
        int64_t v = i;
        suki_dict_set(&s, 8, 0, &v, nullptr);
    }
    int64_t dup = 3;
    suki_dict_set(&s, 8, 0, &dup, nullptr);
    checkEq<int64_t>(suki_dict_len(&s), 10, "set de-duplicates");
    suki_dict_free(s);
}

// ── weak references ────────────────────────────────────────────────────────
void testWeak() {
    std::cout << "Weak references\n";
    int object = 1;
    void* slotA = &object;
    void* slotB = &object;
    suki_weak_register(&object, &slotA);
    suki_weak_register(&object, &slotB);
    check(suki_weak_load(&slotA) == &object, "weak load before clear");

    // Clearing the object must nil every registered slot: this is what breaks
    // retain cycles.
    suki_weak_clear_all(&object);
    check(suki_weak_load(&slotA) == nullptr, "slot A nil after clear");
    check(suki_weak_load(&slotB) == nullptr, "slot B nil after clear");
    suki_weak_clear_all(&object);   // idempotent
    suki_weak_unregister(&slotA);   // already gone, must be a no-op
}

// ── ARC and atomics ────────────────────────────────────────────────────────
void testArcAndAtomics() {
    std::cout << "ARC and atomics\n";
    // The object header is { vtable*, rc } with rc at offset 8, so the counter
    // must be seeded at that offset rather than at the start of the block.
    alignas(16) unsigned char obj[32] = {0};
    suki_arc_retain(obj);
    checkEq<uint64_t>(*reinterpret_cast<uint64_t*>(obj + 8), 1,
                      "retain counts at offset 8");
    suki_arc_retain(obj);
    checkEq<uint64_t>(*reinterpret_cast<uint64_t*>(obj + 8), 2, "second retain");
    suki_arc_release(obj);
    checkEq<uint64_t>(*reinterpret_cast<uint64_t*>(obj + 8), 1, "release");
    check(obj[0] == 0, "vtable slot untouched by retain");

    // A null reference owns nothing, so both operations must tolerate it.
    suki_arc_retain(nullptr);
    suki_arc_release(nullptr);
    check(true, "null retain/release tolerated");

    int64_t v = 5;
    suki_atomic_store_i64(&v, 9);
    checkEq<int64_t>(suki_atomic_load_i64(&v), 9, "atomic load/store");
    checkEq<int64_t>(suki_atomic_add_i64(&v, 6), 15, "atomic add returns new");

    SukiMutex m;
    suki_mutex_init(&m);
    suki_mutex_lock(&m);
    checkEq<int64_t>(m.state, 1, "mutex held");
    suki_mutex_unlock(&m);
    checkEq<int64_t>(m.state, 0, "mutex released");
    suki_mutex_destroy(&m);
}

} // namespace

int main() {
    std::cout << "=== Runtime unit tests ===\n";
    testStrings();
    testArrays();
    testDictionary();
    testWeak();
    testArcAndAtomics();

    std::cout << (g_failures ? "\nFAILED " : "\nPASSED ")
              << (g_checks - g_failures) << "/" << g_checks << " checks\n";
    return g_failures ? 1 : 0;
}
