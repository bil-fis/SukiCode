#include "compiler/codegen/TypeLayout.h"

namespace suki {

std::vector<const TypeRecord::Member*> TypeLayout::valueMembers(const TypeRecord* rec) {
    std::vector<const TypeRecord::Member*> out;
    if (!rec) return out;
    for (const auto& m : rec->members) {
        if (m.isFunction) continue; // methods live in the vtable, not in storage
        out.push_back(&m);
    }
    return out;
}

llvm::StructType* TypeLayout::identified(const std::string& name) {
    auto it = named_.find(name);
    if (it != named_.end()) return it->second;
    // Identified structs are uniqued per context; reuse an existing one so we
    // never emit two distinct LLVM types sharing a name.
    if (llvm::StructType* existing = llvm::StructType::getTypeByName(ctx_, name)) {
        named_[name] = existing;
        return existing;
    }
    llvm::StructType* st = llvm::StructType::create(ctx_, name);
    named_[name] = st;
    return st;
}

// ── shared runtime aggregates ──────────────────────────────────────────────
llvm::Type* TypeLayout::stringTy() {
    if (!stringTy_) {
        stringTy_ = identified("SukiString");
        if (stringTy_->isOpaque())
            stringTy_->setBody({ i8Ptr(), llvm::Type::getInt64Ty(ctx_) });
    }
    return stringTy_;
}

llvm::Type* TypeLayout::arrayTy(llvm::Type* elem) {
    return llvm::StructType::get(ctx_, { llvm::PointerType::getUnqual(elem),
                                        llvm::Type::getInt64Ty(ctx_), // len
                                        llvm::Type::getInt64Ty(ctx_) }); // cap
}

llvm::Type* TypeLayout::dictTy(llvm::Type* key, llvm::Type* value) {
    return llvm::StructType::get(ctx_, { i8Ptr(),                       // entries
                                        llvm::Type::getInt64Ty(ctx_), // len
                                        llvm::Type::getInt64Ty(ctx_), // cap
                                        i8Ptr() });                    // keys
}

llvm::Type* TypeLayout::closureTy(llvm::Type* fnTy) {
    (void)fnTy;
    llvm::StructType* st = identified("SukiClosure");
    if (st->isOpaque()) st->setBody({ i8Ptr(), i8Ptr() }); // captures, fnptr
    return st;
}

llvm::Type* TypeLayout::optionalTy(llvm::Type* payload) {
    // Aggregates and pointers are boxed so an Optional stays pointer-sized;
    // scalars are stored inline next to the flag.
    if (payload->isAggregateType() || payload->isPointerTy())
        return llvm::StructType::get(ctx_, { llvm::PointerType::getUnqual(payload),
                                            llvm::Type::getInt1Ty(ctx_) });
    return llvm::StructType::get(ctx_, { payload, llvm::Type::getInt1Ty(ctx_) });
}

// ── named types ────────────────────────────────────────────────────────────
static bool isRefKind(TypeDeclKind k) {
    return k == TypeDeclKind::Class || k == TypeDeclKind::Actor;
}

llvm::Type* TypeLayout::lowerRecord(const TypeRecord* rec) {
    if (!rec) return i8Ptr();
    const bool isRef = isRefKind(rec->kind);
    auto wrap = [&](llvm::StructType* st) -> llvm::Type* {
        return isRef ? static_cast<llvm::Type*>(llvm::PointerType::getUnqual(st))
                     : static_cast<llvm::Type*>(st);
    };

    auto it = recordTypes_.find(rec);
    if (it != recordTypes_.end()) return wrap(it->second);

    // Create the (opaque) identified struct *before* filling in the body so a
    // self-referential field resolves to this very type instead of recursing.
    llvm::StructType* st = identified("suki." + rec->name);
    recordTypes_[rec] = st;
    defineRecord(rec, st);
    return wrap(st);
}

void TypeLayout::defineRecord(const TypeRecord* rec, llvm::StructType* st) {
    if (!st->isOpaque()) return; // body already set
    std::vector<llvm::Type*> body;
    const bool isRef = isRefKind(rec->kind);

    if (isRef) {
        // Object header, shared with the runtime (see runtime.c):
        //   0 : vtable*    dynamic dispatch
        //   1 : int64      strong reference count
        //   2 : i8*        destructor (deinit), null when the class has none
        // The destructor slot lets the runtime run `deinit` when the last
        // strong reference goes away, without knowing the concrete type.
        body.push_back(i8Ptr());                        // vtable pointer
        body.push_back(llvm::Type::getInt64Ty(ctx_));  // ARC retain count
        body.push_back(i8Ptr());                        // deinit function
        // Actors carry one extra header word: the isolation lock (规范 7.4).
        // Callers acquire it around a cross-actor call so the actor's state is
        // only ever touched by one caller at a time.
        if (rec->kind == TypeDeclKind::Actor)
            body.push_back(llvm::Type::getInt64Ty(ctx_));
    }
    // Single inheritance: superclass storage precedes the subclass fields.
    // 仅嵌入「父类的值成员」，并逐级展开所有祖先——切勿嵌入父类的完整结构体
    // （那会重复祖先头部 vtable/rc/deinit 三个词）。头部在子类统一布局一次。
    if (isRef && rec->superclass) {
        for (const TypeRecord* s = rec->superclass; s; s = s->superclass)
            for (const TypeRecord::Member* sm : valueMembers(s))
                body.push_back(lower(sm->type));
    }
    for (const TypeRecord::Member* m : valueMembers(rec))
        body.push_back(lower(m->type));

    st->setBody(body);
}

// ── the core query ─────────────────────────────────────────────────────────
llvm::Type* TypeLayout::lower(const Type* t) {
    if (!t) return i8Ptr();
    auto& C = ctx_;
    switch (t->kind) {
        case TypeKind::Void:  return llvm::Type::getVoidTy(C);
        case TypeKind::Never: return llvm::Type::getVoidTy(C);
        case TypeKind::Bool:  return llvm::Type::getInt1Ty(C);
        // Char is a Unicode scalar value (21-bit), stored in an i32.
        case TypeKind::Char:  return llvm::Type::getInt32Ty(C);
        case TypeKind::Int8:  return llvm::Type::getInt8Ty(C);
        case TypeKind::Int16: return llvm::Type::getInt16Ty(C);
        case TypeKind::Int32: return llvm::Type::getInt32Ty(C);
        case TypeKind::Int64: return llvm::Type::getInt64Ty(C);
        case TypeKind::UInt8: return llvm::Type::getInt8Ty(C);
        case TypeKind::UInt16:return llvm::Type::getInt16Ty(C);
        case TypeKind::UInt32:return llvm::Type::getInt32Ty(C);
        case TypeKind::UInt64:return llvm::Type::getInt64Ty(C);
        // Platform-sized integers follow the target pointer width.
        case TypeKind::Int: case TypeKind::ISize:
        case TypeKind::UInt: case TypeKind::USize:
            return target_.pointerWidth == 32 ? llvm::Type::getInt32Ty(C)
                                              : llvm::Type::getInt64Ty(C);
        case TypeKind::Float16:  return llvm::Type::getHalfTy(C);
        case TypeKind::Float32:  return llvm::Type::getFloatTy(C);
        case TypeKind::Float64:  return llvm::Type::getDoubleTy(C);
        case TypeKind::Float128: return llvm::Type::getFP128Ty(C);
        case TypeKind::String:   return stringTy();
        // Type-erased boxes / opaque handles.
        case TypeKind::Any: case TypeKind::AnyObject:
        case TypeKind::Error: case TypeKind::Unknown:
        case TypeKind::Metatype:
            return i8Ptr();
        case TypeKind::Named: {
            if (t->record && t->record->kind == TypeDeclKind::Enum) {
                // Raw-valued enums lower to their raw integer; enums carrying
                // associated values are a tag plus a boxed payload.
                bool hasPayload = false;
                for (const auto& c : t->record->cases)
                    if (!c.associated.empty()) { hasPayload = true; break; }
                if (!hasPayload) return llvm::Type::getInt64Ty(C);
                return llvm::StructType::get(C, { llvm::Type::getInt64Ty(C), i8Ptr() });
            }
            return lowerRecord(t->record);
        }
        case TypeKind::Optional: return optionalTy(lower(t->element));
        case TypeKind::Array:    return arrayTy(lower(t->element));
        case TypeKind::Set:      return dictTy(lower(t->element), i8Ptr());
        case TypeKind::Dict:     return dictTy(lower(t->key), lower(t->value));
        case TypeKind::Tuple: {
            std::vector<llvm::Type*> elems;
            elems.reserve(t->elements.size());
            for (const Type* e : t->elements) elems.push_back(lower(e));
            if (elems.empty()) return llvm::StructType::get(C, false);
            return llvm::StructType::get(C, elems);
        }
        case TypeKind::Function: {
            // A function used as a *value* (a parameter, a variable, a return
            // type) is a closure: `{ captures, fnptr }`. Returning the raw
            // llvm::FunctionType would be invalid — a function type cannot be
            // the type of a value, only of a callee.
            std::vector<llvm::Type*> ps;
            ps.reserve(t->elements.size());
            for (const Type* e : t->elements) ps.push_back(lower(e));
            return closureTy(llvm::FunctionType::get(lower(t->ret), ps,
                                                     /*isVarArg=*/false));
        }
        case TypeKind::Closure: {
            std::vector<llvm::Type*> ps;
            ps.reserve(t->elements.size());
            for (const Type* e : t->elements) ps.push_back(lower(e));
            return closureTy(llvm::FunctionType::get(lower(t->ret), ps, false));
        }
        case TypeKind::Ref: {
            // `&T` / `inout T` / `weak T` / `unowned T` are all addresses of the
            // pointee; `Owned<T>` additionally carries a moved flag.
            if (t->refKind == RefKind::Owned)
                return llvm::StructType::get(C, { lower(t->element),
                                                  llvm::Type::getInt1Ty(C) });
            return llvm::PointerType::getUnqual(lower(t->element));
        }
        case TypeKind::Future:
            // A Future<R> handle is an opaque pointer to the boxed result.
            return i8Ptr();
        case TypeKind::OpaquePointer:
            // An untyped runtime handle (Channel / Task / Future) is a raw pointer.
            return i8Ptr();
    }
    return i8Ptr();
}

// ── queries ────────────────────────────────────────────────────────────────
int TypeLayout::fieldIndex(const TypeRecord* rec, const std::string& name) const {
    if (!rec) return -1;
    int idx = 0;
    if (isRefKind(rec->kind)) {
        idx = 3; // skip vtable + retain count + deinit slot
        if (rec->kind == TypeDeclKind::Actor) idx = 4; // ...plus the isolation lock
        // 逐级累加所有祖先的值成员数（与 defineRecord 的父类布局保持一致）。
        for (const TypeRecord* s = rec->superclass; s; s = s->superclass)
            idx += static_cast<int>(valueMembers(s).size());
    }
    for (const TypeRecord::Member* m : valueMembers(rec)) {
        if (m->name == name) return idx;
        ++idx;
    }
    return -1;
}

uint64_t TypeLayout::fieldOffset(llvm::StructType* st, unsigned index) const {
    if (!st || !dl_ || index >= st->getNumElements()) return 0;
    // Honour the target DataLayout instead of assuming a packed layout: the
    // gaps between fields depend on each field's ABI alignment.
    return dl_->getStructLayout(st)->getElementOffset(index);
}

bool TypeLayout::isReferenceType(const Type* t) const {
    return t && t->kind == TypeKind::Named && t->record && isRefKind(t->record->kind);
}

bool TypeLayout::isAggregate(const Type* t) const {
    if (!t) return false;
    switch (t->kind) {
        case TypeKind::Tuple: case TypeKind::Optional:
        case TypeKind::String: case TypeKind::Array:
        case TypeKind::Dict: case TypeKind::Set:
            return true;
        case TypeKind::Named: {
            if (!t->record) return false;
            // class/actor are referenced by pointer, so they are not aggregates.
            if (isRefKind(t->record->kind)) return false;
            if (t->record->kind == TypeDeclKind::Enum) {
                for (const auto& c : t->record->cases)
                    if (!c.associated.empty()) return true;
                return false; // raw-valued enum is a plain integer
            }
            return true; // struct
        }
        default:
            return false;
    }
}

} // namespace suki
