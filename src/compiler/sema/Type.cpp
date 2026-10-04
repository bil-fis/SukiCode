#include "compiler/sema/Type.h"

#include <algorithm>

namespace suki {

// ─── TypeContext ───────────────────────────────────────────────────────────
TypeContext::TypeContext() {
    // Allocate the primitive singletons we cache.
    auto mk = [&](TypeKind k) -> const Type* {
        auto t = std::make_unique<Type>();
        t->kind = k;
        const Type* p = t.get();
        owned_.push_back(std::move(t));
        return p;
    };
    // Fill the primitive cache for every primitive kind in the enum range.
    for (int i = 0; i <= static_cast<int>(TypeKind::Metatype); ++i) {
        TypeKind k = static_cast<TypeKind>(i);
        switch (k) {
            case TypeKind::Void: case TypeKind::Bool: case TypeKind::Char:
            case TypeKind::String: case TypeKind::Int: case TypeKind::UInt:
            case TypeKind::Int8: case TypeKind::Int16: case TypeKind::Int32:
            case TypeKind::Int64: case TypeKind::UInt8: case TypeKind::UInt16:
            case TypeKind::UInt32: case TypeKind::UInt64: case TypeKind::Float16:
            case TypeKind::Float32: case TypeKind::Float64: case TypeKind::Float128:
            case TypeKind::ISize: case TypeKind::USize: case TypeKind::Never:
            case TypeKind::Unknown: case TypeKind::Error: case TypeKind::Any:
            case TypeKind::AnyObject:
                prim_[i] = mk(k);
                break;
            default:
                prim_[i] = nullptr; // compound kinds are built on demand
                break;
        }
    }
}

const Type* TypeContext::alloc(Type t) {
    auto up = std::make_unique<Type>(std::move(t));
    const Type* p = up.get();
    owned_.push_back(std::move(up));
    return p;
}

const Type* TypeContext::intern(const Type& t) {
    for (const Type* e : interned_) {
        if (e->kind != t.kind) continue;
        if (e->record != t.record) continue;
        if (e->name != t.name) continue;
        if (e->element != t.element) continue;
        if (e->key != t.key) continue;
        if (e->value != t.value) continue;
        if (e->ret != t.ret) continue;
        if (e->refKind != t.refKind) continue;
        if (e->elements != t.elements) continue;
        return e;
    }
    const Type* p = alloc(t);
    interned_.push_back(p);
    return p;
}

const Type* TypeContext::optional(const Type* t) {
    Type o; o.kind = TypeKind::Optional; o.element = t; return intern(o);
}
const Type* TypeContext::array(const Type* elem) {
    Type a; a.kind = TypeKind::Array; a.element = elem; a.name = "Array"; return intern(a);
}
const Type* TypeContext::dict(const Type* k, const Type* v) {
    Type d; d.kind = TypeKind::Dict; d.key = k; d.value = v; d.name = "Dictionary";
    return intern(d);
}
const Type* TypeContext::set(const Type* elem) {
    Type s; s.kind = TypeKind::Set; s.element = elem; s.name = "Set"; return intern(s);
}
const Type* TypeContext::tuple(std::vector<const Type*> elems,
                               std::vector<std::string> labels) {
    Type t; t.kind = TypeKind::Tuple; t.elements = std::move(elems);
    t.labels = std::move(labels);
    // Labelled tuples are distinct types (`(a: Int)` is not `(Int)`), so only
    // unlabelled tuples may reuse an interned instance.
    if (t.labels.empty()) return intern(t);
    return alloc(t);
}
const Type* TypeContext::function(std::vector<const Type*> params, const Type* ret) {
    Type f; f.kind = TypeKind::Function; f.elements = std::move(params); f.ret = ret;
    return alloc(f);
}
const Type* TypeContext::closure(std::vector<const Type*> params, const Type* ret,
                                 std::vector<std::string> captures) {
    Type c; c.kind = TypeKind::Closure; c.elements = std::move(params); c.ret = ret;
    c.captures = std::move(captures);
    return alloc(c);
}
const Type* TypeContext::ref(RefKind k, const Type* pointee) {
    Type r; r.kind = TypeKind::Ref; r.refKind = k; r.element = pointee; return intern(r);
}
const Type* TypeContext::metatype(const Type* base, bool isProtocol) {
    Type m; m.kind = TypeKind::Metatype; m.element = base; m.refKind =
        isProtocol ? RefKind::Unowned : RefKind::Shared; return intern(m);
}
const Type* TypeContext::future(const Type* result) {
    Type f; f.kind = TypeKind::Future; f.element = result; return intern(f);
}
const Type* TypeContext::named(const TypeRecord* rec, std::string name,
                               std::vector<const Type*> genericArgs) {
    Type n; n.kind = TypeKind::Named; n.record = rec; n.name = std::move(name);
    n.elements = std::move(genericArgs);
    return intern(n);
}

// ─── Utilities ─────────────────────────────────────────────────────────────
bool isIdentical(const Type* a, const Type* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TypeKind::Named:
            if (a->record && b->record) return a->record == b->record;
            return a->name == b->name;
        case TypeKind::Optional: case TypeKind::Array: case TypeKind::Set:
            return isIdentical(a->element, b->element);
        case TypeKind::Ref: case TypeKind::Metatype:
            return a->refKind == b->refKind && isIdentical(a->element, b->element);
        case TypeKind::Future:
            return isIdentical(a->element, b->element);
        case TypeKind::Dict:
            return isIdentical(a->key, b->key) && isIdentical(a->value, b->value);
        case TypeKind::Tuple: case TypeKind::Function: case TypeKind::Closure:
            if (a->ret && b->ret) {
                if (!isIdentical(a->ret, b->ret)) return false;
            } else if (a->ret != b->ret) {
                return false;
            }
            if (a->elements.size() != b->elements.size()) return false;
            for (size_t i = 0; i < a->elements.size(); ++i) {
                if (!isIdentical(a->elements[i], b->elements[i])) return false;
            }
            return true;
        default:
            return true; // primitives / Never / Unknown compare by kind
    }
}

bool isValueType(const Type* t) {
    if (!t) return true;
    switch (t->kind) {
        case TypeKind::Named:
            // A Named type is a value type unless its record says otherwise
            // (class/actor). Without a record (forward-declared) assume value.
            return true;
        case TypeKind::Function: case TypeKind::Metatype: case TypeKind::Future:
            return false;
        default:
            return true;
    }
}

bool isNumeric(const Type* t) {
    if (!t) return false;
    switch (t->kind) {
        case TypeKind::Int: case TypeKind::UInt:
        case TypeKind::Int8: case TypeKind::Int16: case TypeKind::Int32: case TypeKind::Int64:
        case TypeKind::UInt8: case TypeKind::UInt16: case TypeKind::UInt32: case TypeKind::UInt64:
        case TypeKind::Float16: case TypeKind::Float32: case TypeKind::Float64:
        case TypeKind::Float128:
        case TypeKind::ISize: case TypeKind::USize:
            return true;
        default:
            return false;
    }
}

bool builtinTypeFromName(const std::string& name, TypeKind& out) {
    struct Entry { const char* n; TypeKind k; };
    static const Entry table[] = {
        {"Void", TypeKind::Void}, {"Bool", TypeKind::Bool}, {"Char", TypeKind::Char},
        {"String", TypeKind::String}, {"Int", TypeKind::Int}, {"UInt", TypeKind::UInt},
        {"Int8", TypeKind::Int8}, {"Int16", TypeKind::Int16},
        {"Int32", TypeKind::Int32}, {"Int64", TypeKind::Int64},
        {"UInt8", TypeKind::UInt8}, {"UInt16", TypeKind::UInt16},
        {"UInt32", TypeKind::UInt32}, {"UInt64", TypeKind::UInt64},
        {"Float", TypeKind::Float32}, {"Float16", TypeKind::Float16},
        {"Float32", TypeKind::Float32}, {"Float64", TypeKind::Float64},
        {"Double", TypeKind::Float64}, {"Float128", TypeKind::Float128},
        {"ISize", TypeKind::ISize}, {"USize", TypeKind::USize},
        {"Any", TypeKind::Any}, {"AnyObject", TypeKind::AnyObject},
        {"Error", TypeKind::Error},
    };
    for (const auto& e : table) {
        if (name == e.n) { out = e.k; return true; }
    }
    return false;
}

std::string typeToString(const Type* t) {
    if (!t) return "<null>";
    switch (t->kind) {
        case TypeKind::Void: return "Void";
        case TypeKind::Bool: return "Bool";
        case TypeKind::Char: return "Char";
        case TypeKind::String: return "String";
        case TypeKind::Int: return "Int";
        case TypeKind::UInt: return "UInt";
        case TypeKind::Int8: return "Int8";
        case TypeKind::Int16: return "Int16";
        case TypeKind::Int32: return "Int32";
        case TypeKind::Int64: return "Int64";
        case TypeKind::UInt8: return "UInt8";
        case TypeKind::UInt16: return "UInt16";
        case TypeKind::UInt32: return "UInt32";
        case TypeKind::UInt64: return "UInt64";
        case TypeKind::Float16: return "Float16";
        case TypeKind::Float32: return "Float32";
        case TypeKind::Float64: return "Double";
        case TypeKind::Float128: return "Float128";
        case TypeKind::ISize: return "ISize";
        case TypeKind::USize: return "USize";
        case TypeKind::Never: return "!";
        case TypeKind::Unknown: return "<unknown>";
        case TypeKind::Error: return "Error";
        case TypeKind::Any: return "Any";
        case TypeKind::AnyObject: return "AnyObject";
        case TypeKind::Optional:
            return typeToString(t->element) + "?";
        case TypeKind::Array:
            return t->element ? ("[" + typeToString(t->element) + "]") : "[]";
        case TypeKind::Set:
            return "Set<" + (t->element ? typeToString(t->element) : "?") + ">";
        case TypeKind::Dict:
            return "[" + (t->key ? typeToString(t->key) : "?") + ": " +
                   (t->value ? typeToString(t->value) : "?") + "]";
        case TypeKind::Named: {
            std::string s = t->name;
            if (!t->elements.empty()) {
                s += "<";
                for (size_t i = 0; i < t->elements.size(); ++i) {
                    if (i) s += ", ";
                    s += typeToString(t->elements[i]);
                }
                s += ">";
            }
            return s;
        }
        case TypeKind::Tuple: {
            std::string s = "(";
            for (size_t i = 0; i < t->elements.size(); ++i) {
                if (i) s += ", ";
                s += typeToString(t->elements[i]);
            }
            return s + ")";
        }
        case TypeKind::Function: case TypeKind::Closure: {
            std::string s = "(";
            for (size_t i = 0; i < t->elements.size(); ++i) {
                if (i) s += ", ";
                s += typeToString(t->elements[i]);
            }
            s += ") -> ";
            s += t->ret ? typeToString(t->ret) : "Void";
            return s;
        }
        case TypeKind::Ref: {
            switch (t->refKind) {
                case RefKind::Shared: return "&" + typeToString(t->element);
                case RefKind::Mut: return "inout " + typeToString(t->element);
                case RefKind::Weak: return "weak " + typeToString(t->element);
                case RefKind::Unowned: return "unowned " + typeToString(t->element);
                case RefKind::Owned: return "Owned<" + typeToString(t->element) + ">";
            }
            return typeToString(t->element);
        }
        case TypeKind::Metatype:
            return typeToString(t->element) +
                   (t->refKind == RefKind::Unowned ? ".Protocol" : ".Type");
        case TypeKind::Future:
            return "Future<" + (t->element ? typeToString(t->element) : "?") + ">";
    }
    return "<type>";
}

} // namespace suki
