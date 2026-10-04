#pragma once

// Semantic type representation for SukiCode.
//
// The parser produces syntactic `TypeRepr` nodes (see ast/AST.h). This header
// defines the *semantic* types the analyzer and later stages operate on:
// immutable, structurally-comparable type objects allocated by a `TypeContext`.
//
// All types are `const Type*` and are owned by the TypeContext that created
// them, so they live for the duration of a compilation session.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace suki {

struct TypeRecord; // defined in sema/Sema.h — the declaration behind a Named type

// ─── Type kinds ───────────────────────────────────────────────────────────
enum class TypeKind {
    // primitives / builtin
    Void, Bool, Char, String,
    Int, UInt,                     // platform-sized (64-bit on our targets)
    Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64,
    Float16, Float32, Float64, Float128,
    ISize, USize,
    Never,        // `!` — diverging
    Unknown,      // error-recovery placeholder
    Error,        // the `Error` protocol (throwable)
    Any, AnyObject,
    // compound
    Named,        // struct / enum / class / actor / protocol / typealias
    Optional,     // T?
    Array,        // Array<T> / [T]
    Dict,         // Dictionary<K,V> / [K:V]
    Set,          // Set<T>
    Tuple,        // (A, B, ...)
    Function,     // (A, B) -> R
    Closure,      // closure literal type (params, ret, captures)
    Ref,          // &T (shared), inout T, weak/unowned/owned wrappers
    Metatype,     // T.Type / T.Protocol
};

enum class RefKind {
    Shared,    // &T
    Mut,       // inout T
    Weak,      // weak T (ARC, nils out)
    Unowned,   // unowned T (ARC, non-optional)
    Owned,     // Owned<T> (unique ownership / move)
};

// ─── Type ──────────────────────────────────────────────────────────────────
struct Type {
    TypeKind kind = TypeKind::Unknown;

    // Named: the record describing the declaration + its generic arguments.
    const TypeRecord* record = nullptr;
    std::string name;                          // Named (and diagnostics)

    // Optional/Array/Set/Ref/Metatype element or pointee.
    const Type* element = nullptr;
    // Dict key/value.
    const Type* key = nullptr;
    const Type* value = nullptr;
    // Tuple elements, Function/Closure params, Named generic args.
    std::vector<const Type*> elements;
    // Tuple element labels (`(code: 200, message: "OK")`), parallel to
    // `elements`; an empty string means the element is unlabelled. Used to
    // resolve `pair.code` to an element index.
    std::vector<std::string> labels;
    // Function/Closure return.
    const Type* ret = nullptr;
    // Ref wrapper kind.
    RefKind refKind = RefKind::Shared;
    // Closure capture descriptors (unused for now; reserved).
    std::vector<std::string> captures;
};

// ─── Type factory ──────────────────────────────────────────────────────────
// Owns every Type instance and hands out `const Type*` pointers. One context
// lives per compilation session.
class TypeContext {
public:
    TypeContext();
    // Canonical primitives (allocated once, cached).
    const Type* voidType() const { return prim_[static_cast<int>(TypeKind::Void)]; }
    const Type* boolType() const { return prim_[static_cast<int>(TypeKind::Bool)]; }
    const Type* charType() const { return prim_[static_cast<int>(TypeKind::Char)]; }
    const Type* stringType() const { return prim_[static_cast<int>(TypeKind::String)]; }
    const Type* intType() const { return prim_[static_cast<int>(TypeKind::Int)]; }
    const Type* doubleType() const { return prim_[static_cast<int>(TypeKind::Float64)]; }
    const Type* neverType() const { return prim_[static_cast<int>(TypeKind::Never)]; }
    const Type* unknownType() const { return prim_[static_cast<int>(TypeKind::Unknown)]; }
    const Type* errorType() const { return prim_[static_cast<int>(TypeKind::Error)]; }
    const Type* anyType() const { return prim_[static_cast<int>(TypeKind::Any)]; }
    const Type* anyObjectType() const { return prim_[static_cast<int>(TypeKind::AnyObject)]; }
    // Generic primitive accessor by kind (must be a primitive kind).
    const Type* primitive(TypeKind k) const { return prim_[static_cast<int>(k)]; }

    // Compound constructors.
    const Type* optional(const Type* t);
    const Type* array(const Type* elem);
    const Type* dict(const Type* k, const Type* v);
    const Type* set(const Type* elem);
    const Type* tuple(std::vector<const Type*> elems,
                      std::vector<std::string> labels = {});
    const Type* function(std::vector<const Type*> params, const Type* ret);
    const Type* closure(std::vector<const Type*> params, const Type* ret,
                        std::vector<std::string> captures = {});
    const Type* ref(RefKind k, const Type* pointee);
    const Type* metatype(const Type* base, bool isProtocol);
    const Type* named(const TypeRecord* rec, std::string name,
                      std::vector<const Type*> genericArgs = {});

    // Interning (optional; structurally equal compounds are reused).
    const Type* intern(const Type& t);

private:
    const Type* alloc(Type t);

    std::vector<std::unique_ptr<Type>> owned_;
    // Cache for primitive singletons indexed by TypeKind.
    const Type* prim_[static_cast<int>(TypeKind::Metatype) + 1] = {};
    // Simple structural cache for optional/array/ref/... to limit growth.
    std::vector<const Type*> interned_;
};

// ─── Utilities ─────────────────────────────────────────────────────────────
// Structural identity (two named types are identical iff same record).
bool isIdentical(const Type* a, const Type* b);
// Human-readable rendering used in diagnostics.
std::string typeToString(const Type* t);
// True for value semantics (struct/enum/primitives/tuple/array/set/dict/optional).
bool isValueType(const Type* t);
// True if `t` is an integer or floating-point type (for operator checking).
bool isNumeric(const Type* t);
// Builtin name → TypeKind; returns false if `name` is not a builtin.
bool builtinTypeFromName(const std::string& name, TypeKind& out);

} // namespace suki
