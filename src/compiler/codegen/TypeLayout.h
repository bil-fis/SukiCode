#pragma once

// TypeLayout — the single authority for SukiCode's LLVM data representation.
//
// Every other lowering stage (values, classes, enums, generics, closures)
// asks this class "what LLVM type represents a value of T?", so the
// representation can never drift between stages.
//
// Representation (chosen per the language spec, and mirrored by the C runtime
// ABI in src/runtime/):
//
//   Int / UInt / Int64 ...   i64 / i64 / i64 ...      fixed-width integers
//   Float / Double           float / double
//   Bool                     i1
//   Char                     i32                     Unicode scalar (21-bit)
//   String                   %SukiString = { i8*, i64 }
//   Array<T>                 %SukiArray  = { T*, i64 len, i64 cap }
//   Dictionary<K,V>          %SukiDict   = { i8*, i64 len, i64 cap, i8* keys }
//   Set<T>                   %SukiDict   (uniqueness is a runtime invariant)
//   struct T                 %T = type { fields... }         value semantics
//   class / actor T          %T = type { i8** vtable, i64 rc, fields... }
//                            used as %T*                       ARC-managed
//   enum with payloads       %E = type { i64 tag, i8* payload }
//   enum raw-valued          the raw integer type
//   T?                       { T, i1 }  (boxed to a pointer for large T)
//   tuple                    { elems... }
//   Any / AnyObject          i8*        (type-erased box)
//   closure                  %SukiClosure = { i8* captures, fnptr }
//
// Named types become *identified* structs (StructType::create(ctx, name)),
// which are uniqued per LLVMContext. We create the (opaque) type first and fill
// in its body afterwards, which is what makes recursive types — a class with a
// field of its own type, `class Node { var next: Node? }` — expressible.
//
// Field offsets are never hand-computed: struct layout follows the target
// DataLayout, so padding between fields is honoured rather than assumed.

#include "compiler/codegen/TargetInfo.h"
#include "compiler/sema/Sema.h" // TypeRecord
#include "compiler/sema/Type.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace suki {

class TypeLayout {
public:
    TypeLayout(llvm::LLVMContext& ctx, const TargetInfo& target, const llvm::DataLayout* dl)
        : ctx_(ctx), target_(target), dl_(dl) {}

    // ── the core query ────────────────────────────────────────────────────
    // LLVM type representing a *value* of `t`. Reference types (class/actor)
    // yield a pointer to their object; value types are the structure itself.
    llvm::Type* lower(const Type* t);
    llvm::Type* lowerRecord(const TypeRecord* rec);

    // The identified struct for a named type, or null if not lowered yet.
    llvm::StructType* objectType(const TypeRecord* rec) const {
        auto it = recordTypes_.find(rec);
        return it == recordTypes_.end() ? nullptr : it->second;
    }

    // Index of the value member `name` in `rec`, or -1 if absent.
    int fieldIndex(const TypeRecord* rec, const std::string& name) const;

    // Byte offset of field `index` inside `st`, honouring DataLayout padding.
    uint64_t fieldOffset(llvm::StructType* st, unsigned index) const;

    // Convenience handles on the shared runtime aggregates.
    llvm::Type* i8Ptr() const { return llvm::PointerType::get(ctx_, 0); }
    llvm::Type* stringTy();
    llvm::Type* arrayTy(llvm::Type* elem);
    llvm::Type* dictTy(llvm::Type* key, llvm::Type* value);
    llvm::Type* closureTy(llvm::Type* fnTy);
    llvm::Type* optionalTy(llvm::Type* payload);

    // True when a value of `t` is ARC-managed and therefore needs retain /
    // release around assignment, argument passing and scope exit.
    bool isReferenceType(const Type* t) const;

    // True when a value of `t` lives in memory and must be addressed with a
    // GEP (aggregates: structs, enums, tuples, options, strings, collections).
    bool isAggregate(const Type* t) const;

    // Value members of a record, i.e. the stored properties that participate
    // in the object layout (methods, initialisers and cases are excluded).
    static std::vector<const TypeRecord::Member*> valueMembers(const TypeRecord* rec);

    const TargetInfo& target() const { return target_; }

private:
    llvm::StructType* identified(const std::string& name);
    void defineRecord(const TypeRecord* rec, llvm::StructType* st);

    llvm::LLVMContext& ctx_;
    const TargetInfo& target_;
    const llvm::DataLayout* dl_;
    std::unordered_map<const TypeRecord*, llvm::StructType*> recordTypes_;
    std::unordered_map<std::string, llvm::StructType*> named_;
    llvm::StructType* stringTy_ = nullptr;
};

} // namespace suki
