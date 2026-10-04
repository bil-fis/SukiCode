#include "compiler/sema/Sema.h"

#include <algorithm>
#include <map>

namespace suki {

static SourceRange rangeOf(Node* n) { return n ? n->range : SourceRange{}; }


// Check each recorded generic instantiation with its type arguments bound.
void Sema::monomorphise(const NodeList& decls) {
    // Index the generic functions by name so an instance can find its body.
    std::unordered_map<std::string, FunctionDecl*> genericFns;
    for (auto& d : decls) {
        if (!d || d->kind != NodeKind::FunctionDecl) continue;
        auto* fn = static_cast<FunctionDecl*>(d.get());
        if (!fn->genericParams.empty()) genericFns[fn->name] = fn;
    }
    if (genericFns.empty()) return;

    // Reset the error state so a body that only fails for some instantiation
    // does not abort the others; the diagnostic is deduplicated on emit.
    const std::vector<GenericInstance> instances = genericInstances_;
    for (const GenericInstance& gi : instances) {
        auto fit = genericFns.find(gi.funcName);
        if (fit == genericFns.end()) continue;
        FunctionDecl* fn = fit->second;
        if (gi.typeArgs.size() != fn->genericParams.size()) continue;

        std::unordered_map<std::string, const Type*> saved = genericBindings_;
        for (size_t i = 0; i < gi.typeArgs.size(); ++i)
            genericBindings_[fn->genericParams[i]] = typeByName(gi.typeArgs[i]);
        // Re-check the body with the parameters bound, so each instantiation is
        // verified against the types it will actually be used with.
        checkFunctionBody(fn, nullptr);
        genericBindings_ = saved;
    }
}

// Resolve a written type name the way an annotation would. An unknown name (a
// type parameter used as an argument, for instance) stays opaque.
const Type* Sema::typeByName(const std::string& name) {

    if (const TypeRecord* rec = findType(name))
        return types_.named(rec, name);
    TypeKind bk;
    if (builtinTypeFromName(name, bk)) {
        if (bk == TypeKind::Array) return types_.array(types_.unknownType());
        return types_.primitive(bk);
    }
    return types_.unknownType();
}

void Sema::bindInstance(size_t index, FunctionDecl* fn) {
    bindingStack_.push_back(genericBindings_);
    if (index >= genericInstances_.size() || !fn) return;
    const GenericInstance& gi = genericInstances_[index];
    if (gi.funcName != fn->name) return;
    if (gi.typeArgs.size() != fn->genericParams.size()) return;
    for (size_t i = 0; i < gi.typeArgs.size(); ++i)
        genericBindings_[fn->genericParams[i]] = typeByName(gi.typeArgs[i]);
    // Parameter types were fixed during collection, when the parameters were
    // still opaque. Re-resolve them now so the signature matches this instance.
    for (Param& prm : fn->params) {
        prm.semaType = prm.type ? resolveTypeRepr(prm.type.get(), nullptr) : nullptr;
        std::string d; if (prm.type && prm.type->kind == NodeKind::ArrayType)
            d = typeToString(prm.semaType);
    }
    checkFunctionBody(fn, nullptr);
}

void Sema::unbindInstance() {
    if (bindingStack_.empty()) return;
    genericBindings_ = bindingStack_.back();
    bindingStack_.pop_back();
}

// The type a generic argument should be inferred as. A parameter written `[T]`
// or `Dictionary<K,V>` is matched by its element(s), not by the container, which
// is what the instantiation has to be named after.
const Type* Sema::inferTypeArgument(const Type* t) {
    if (!t) return t;
    switch (t->kind) {
        case TypeKind::Array: case TypeKind::Optional:
            return t->element ? t->element : t;
        case TypeKind::Set:
            return t->element ? t->element : t;
        case TypeKind::Dict:
            // A two-parameter function is instantiated per key/value pair; the
            // caller splits them, so a dict contributes its key here.
            return t->key ? t->key : t;
        default: return t;
    }
}

void Sema::checkInstance(size_t index, FunctionDecl* fn) {
    bindInstance(index, fn);
    unbindInstance();
}

void Sema::recordGenericInstance(const std::string& funcName,
                                const std::vector<std::string>& typeArgs) {
    for (const GenericInstance& gi : genericInstances_)
        if (gi.funcName == funcName && gi.typeArgs == typeArgs) return;
    genericInstances_.push_back(GenericInstance{funcName, typeArgs});
}

// Report class properties that form a retain cycle.
//
// A class is a node; a strong reference from one class to another is an edge.
// Any cycle means two objects keep each other alive, so neither can be
// collected. The language does not silently downgrade such a reference to
// `unowned` — that would change the programmer's intent — so this is a
// diagnostic naming the property to make `weak` or `unowned`.
void Sema::checkReferenceCycles() {
    std::map<std::string, std::vector<std::string>> edges; // class -> classes
    std::map<std::string, const TypeRecord*> byName;
    for (const auto& kv : typeIndex_) byName[kv.first] = kv.second;

    for (const auto& kv : typeIndex_) {
        const TypeRecord* rec = kv.second;
        if (rec->kind != TypeDeclKind::Class && rec->kind != TypeDeclKind::Actor)
            continue;
        for (const auto& m : rec->members) {
            // A `weak` / `unowned` property does not retain, so it is not an edge.
            if (m.decl && m.decl->kind == NodeKind::VarDecl) {
                auto* vd = static_cast<VarDecl*>(m.decl);
                if (vd->isWeak || vd->isUnowned) continue;
            }
            if (!m.type || m.type->kind != TypeKind::Named || !m.type->record)
                continue;
            const TypeRecord* target = m.type->record;
            if (target->kind != TypeDeclKind::Class &&
                target->kind != TypeDeclKind::Actor)
                continue;
            edges[rec->name].push_back(target->name);
        }
    }
    if (edges.empty()) return;

    // Iterative depth-first search with an explicit stack: recursion would risk
    // deep stacks on long inheritance chains, and the graph is small but cyclic.
    std::map<std::string, int> state; // 0 unvisited, 1 on stack, 2 done
    std::vector<std::pair<std::string, size_t>> stack;
    for (const auto& kv : edges) {
        if (state[kv.first]) continue;
        stack.push_back({kv.first, 0});
        state[kv.first] = 1;
        while (!stack.empty()) {
            auto& top = stack.back();
            const std::vector<std::string>& outs = edges[top.first];
            if (top.second < outs.size()) {
                const std::string next = outs[top.second++];
                if (state[next] == 1) {
                    // Found a back edge: report the property that closes it.
                    const std::string& from = top.first;
                    for (const auto& m : byName[from]->members)
                        if (m.type && m.type->name == next)
                            diags_.reportWarning("class '" + from + "' has a strong reference to '" +
                                          next + "' through '" + m.name +
                                          "', forming a retain cycle; declare it "
                                          "'weak' or 'unowned' to break the cycle",
                                          m.decl ? rangeOf(m.decl) : SourceRange());
                } else if (state[next] == 0) {
                    state[next] = 1;
                    stack.push_back({next, 0});
                }
            } else {
                state[top.first] = 2;
                stack.pop_back();
            }
        }
    }
}

// Check `e` where a `Char` is expected: a one-scalar string literal denotes a
// character, anything else keeps its own type.
const Type* Sema::checkAsChar(Node* e, const TypeRecord* context) {
    if (!e || e->kind != NodeKind::StrLitExpr) return checkExpr(e, context);
    charContext_ = true;
    const Type* t = checkExpr(e, context);
    charContext_ = false;
    return t;
}

void Sema::analyze(const NodeList& decls) {
    hadError_ = false;
    globals_.pushScope();
    locals_.pushScope();
    registerBuiltins();

    // Pass 1: create type records for every named type declaration.
    for (auto& d : decls) {
        if (!d) continue;
        switch (d->kind) {
            case NodeKind::StructDecl: case NodeKind::EnumDecl:
            case NodeKind::ClassDecl: case NodeKind::ActorDecl:
            case NodeKind::ProtocolDecl:
                collectTypeDecl(d.get());
                break;
            case NodeKind::TypealiasDecl:
                collectTypeAlias(static_cast<TypealiasDecl*>(d.get()));
                break;
            default: break;
        }
    }

    // Pass 2: resolve inheritance / conformances by name.
    for (auto& kv : typeIndex_) {
        TypeRecord* rec = kv.second;
        if (!rec->decl) continue;
        auto* td = static_cast<TypeDecl*>(rec->decl);
        for (auto& base : td->inherited) {
            if (!base || base->kind != NodeKind::NamedType) continue;
            auto* nt = static_cast<NamedType*>(base.get());
            const TypeRecord* baseRec = findType(nt->name);
            if (!baseRec) continue;
            if (baseRec->isProtocol()) {
                rec->protocols.push_back(baseRec);
            } else if (rec->kind == TypeDeclKind::Class && !rec->superclass) {
                rec->superclass = baseRec;
            }
        }
    }

    // Pass 3: collect members of every type.
    for (auto& kv : typeIndex_) {
        TypeRecord* rec = kv.second;
        if (rec->decl) collectMembers(*rec, static_cast<TypeDecl*>(rec->decl));
    }

    // Pass 3.5: fold extension members (and the conformances they declare)
    // into the extended type, then inherit protocol default implementations.
    // After this the member list of every type is flat, so code generation
    // and member lookup need no special handling for extensions (规范 2.6/4.5).
    mergeExtensions(decls);
    mergeDefaultImplementations();
    // 继承校验（规范 4.3）：override / final 语义，需在成员收集完成后进行。
    checkOverrides();

    // Pass 4: collect global functions and variables.
    for (auto& d : decls) {
        if (!d) continue;
        if (d->kind == NodeKind::FunctionDecl) {
            collectFunction(static_cast<FunctionDecl*>(d.get()), nullptr);
        } else if (d->kind == NodeKind::VarDecl) {
            collectGlobalVar(static_cast<VarDecl*>(d.get()));
        }
    }

    // Pass 5: check function bodies.
    for (auto& kv : typeIndex_) {
        TypeRecord* rec = kv.second;
        if (!rec->decl) continue;
        auto* td = static_cast<TypeDecl*>(rec->decl);
        for (auto& m : td->members) {
            if (m && m->kind == NodeKind::FunctionDecl) {
                checkFunctionBody(static_cast<FunctionDecl*>(m.get()), rec);
            } else if (m && m->kind == NodeKind::InitDecl) {
                auto* init = static_cast<InitDecl*>(m.get());
                currentType_ = rec;
                currentReturn_ = types_.voidType();
                currentThrows_ = false;
                locals_.pushScope();
                // `self` is implicitly available in methods.
                Symbol self; self.kind = Symbol::Kind::Variable;
                self.type = types_.named(rec, rec->name);
                self.decl = m.get();
                locals_.declare("self", self);
                for (auto& prm : init->params) {
                    Symbol s; s.kind = Symbol::Kind::Parameter;
                    s.type = resolveTypeRepr(prm.type.get(), rec);
                    s.decl = m.get();
                    locals_.declare(prm.internalName, s);
                }
                checkStatements(init->body, rec, types_.voidType(), false);
                locals_.popScope();
                currentType_ = nullptr;
            } else if (m && m->kind == NodeKind::SubscriptDecl) {
                // 自定义下标（规范 3.1）：get/set 体内 `self`、下标参数、`newValue`
                // 可用，且隐式 self 成员需解析（与访问器一致）。
                auto* sub = static_cast<SubscriptDecl*>(m.get());
                const Type* elemT = sub->elementType
                    ? resolveTypeRepr(sub->elementType.get(), rec)
                    : types_.unknownType();
                const TypeRecord* savedType = currentType_;
                currentType_ = rec;
                auto checkSubBody = [&](const NodeList& body, const Type* ret,
                                         bool isSetter) {
                    locals_.pushScope();
                    Symbol self; self.kind = Symbol::Kind::Variable;
                    self.type = types_.named(rec, rec->name); self.decl = m.get();
                    locals_.declare("self", self);
                    for (auto& prm : sub->params) {
                        Symbol s; s.kind = Symbol::Kind::Parameter;
                        s.type = resolveTypeRepr(prm.type.get(), rec);
                        s.decl = m.get();
                        locals_.declare(prm.internalName, s);
                    }
                    if (isSetter) {
                        Symbol nv; nv.kind = Symbol::Kind::Parameter;
                        nv.type = elemT; nv.decl = m.get();
                        locals_.declare("newValue", nv);
                    }
                    checkStatements(body, rec, ret, false);
                    locals_.popScope();
                };
                if (!sub->getter.empty()) checkSubBody(sub->getter, elemT, false);
                if (!sub->setter.empty())
                    checkSubBody(sub->setter, types_.voidType(), true);
                currentType_ = savedType;
            }
        }
    }
    for (auto& d : decls) {
        if (d && d->kind == NodeKind::FunctionDecl) {
            checkFunctionBody(static_cast<FunctionDecl*>(d.get()), nullptr);
        } else if (d && d->kind == NodeKind::VarDecl) {
            checkGlobalVarBody(static_cast<VarDecl*>(d.get()));
        }
    }

    // Monomorphisation: the first pass above checked every generic body with its
    // type parameters still opaque, so nothing concrete was recorded. Now that
    // the call sites have revealed which type arguments are used, check each
    // distinct instantiation again with those parameters bound, which annotates
    // the body with real types for lowering.
    if (!genericInstances_.empty()) monomorphise(decls);

    locals_.popScope();
    globals_.popScope();

    // Trait / protocol conformance checking.
    checkConformances();

    // Retain-cycle detection runs last: it needs every class's property types,
    // which are only final after the bodies above have been checked.
    checkReferenceCycles();
}

void Sema::checkConformances() {
    for (auto& kv : typeIndex_) {
        TypeRecord* rec = kv.second;
        for (const TypeRecord* proto : rec->protocols) {
            if (!proto || !proto->isProtocol()) continue;
            for (const auto& req : proto->requirements) {
                // Default implementation supplied by the protocol itself?
                bool hasDefault = false;
                if (req.isFunction && req.decl &&
                    req.decl->kind == NodeKind::FunctionDecl) {
                    auto* fd = static_cast<FunctionDecl*>(req.decl);
                    hasDefault = !fd->body.empty();
                }
                if (hasDefault) continue;
                // Satisfied by a member on the type or any superclass?
                bool satisfied = false;
                for (const TypeRecord* r = rec; r && !satisfied; r = r->superclass) {
                    if (req.isFunction) {
                        if (lookupMethod(r, req.name, req.type ? req.type->elements.size() : 0))
                            satisfied = true;
                    } else {
                        if (lookupMember(r, req.name, false)) satisfied = true;
                    }
                }
                if (!satisfied) {
                    hadError_ = true;
                    diags_.reportError("type '" + rec->name + "' does not conform to protocol '" +
                                       proto->name + "': missing requirement '" + req.name + "'",
                                       rangeOf(rec->decl));
                }
            }
        }
    }
}

// ─── Collection ────────────────────────────────────────────────────────────
void Sema::registerBuiltins() {
    auto addFn = [&](const char* name, std::vector<const Type*> params, const Type* ret) {
        Symbol s;
        s.kind = Symbol::Kind::Function;
        s.type = types_.function(std::move(params), ret);
        globals_.declare(name, s);
    };
    // Provided by the C runtime (src/runtime/runtime.c).
    // `print` / `println` accept any printable value (规范 12.1), which the
    // `Any` parameter expresses — the code generator formats each scalar type
    // on its own.
    addFn("print", { types_.anyType() }, types_.voidType());
    addFn("println", { types_.anyType() }, types_.voidType());
    addFn("panic", { types_.stringType() }, types_.voidType());
}

void Sema::collectTypeDecl(Node* decl) {
    auto* td = static_cast<TypeDecl*>(decl);
    auto rec = std::make_unique<TypeRecord>();
    rec->name = td->name;
    rec->decl = td;
    switch (td->kind) {
        case NodeKind::StructDecl: rec->kind = TypeDeclKind::Struct; break;
        case NodeKind::EnumDecl: rec->kind = TypeDeclKind::Enum; break;
        case NodeKind::ClassDecl: rec->kind = TypeDeclKind::Class; break;
        case NodeKind::ActorDecl: rec->kind = TypeDeclKind::Actor; break;
        case NodeKind::ProtocolDecl: rec->kind = TypeDeclKind::Protocol; break;
        default: rec->kind = TypeDeclKind::Struct; break;
    }
    rec->genericParams = td->genericParams;

    TypeRecord* raw = rec.get();
    typeIndex_[td->name] = raw;
    // Own the record for the session.
    ownedRecords_.push_back(std::move(rec));

    // Annotate the declaration so the code generator can recover the record
    // from the AST alone (IRGenerator never sees Sema's tables directly).
    td->semaType = types_.named(raw, td->name);

    Symbol s; s.kind = Symbol::Kind::Type; s.record = raw; s.decl = td;
    globals_.declare(td->name, s);
}

const TypeRecord* Sema::findType(const std::string& name) const {
    auto it = typeIndex_.find(name);
    return it == typeIndex_.end() ? nullptr : it->second;
}

void Sema::addMember(TypeRecord& rec, Node* m) {
    if (!m) return;
    if (m->kind == NodeKind::VarDecl || m->kind == NodeKind::BlockStmt) {
        // `var x: T` is a single member; `var x, y: T` desugars to a
        // BlockStmt of VarDecls — register each binding as a property.
        std::vector<VarDecl*> varDecls;
        if (m->kind == NodeKind::VarDecl) {
            varDecls.push_back(static_cast<VarDecl*>(m));
        } else {
            for (auto& sub : static_cast<BlockStmt*>(m)->statements) {
                if (sub && sub->kind == NodeKind::VarDecl)
                    varDecls.push_back(static_cast<VarDecl*>(sub.get()));
            }
        }
        for (VarDecl* vd : varDecls) {
            TypeRecord::Member mem;
            mem.name = vd->name;
            mem.isFunction = false;
            mem.isLet = vd->isLet;
            mem.isStatic = std::find(vd->modifiers.begin(), vd->modifiers.end(),
                                     "static") != vd->modifiers.end();
            mem.decl = vd;
            if (vd->type) mem.type = resolveTypeRepr(vd->type.get(), &rec);
            else if (vd->initializer) mem.type = checkExpr(vd->initializer.get(), &rec);
            else mem.type = types_.unknownType();
            rec.members.push_back(std::move(mem));
        }
    } else if (m->kind == NodeKind::FunctionDecl) {
        auto* fn = static_cast<FunctionDecl*>(m);
        TypeRecord::Member mem;
        mem.name = fn->name;
        mem.isFunction = true;
        mem.decl = m;
        mem.isStatic = std::find(fn->modifiers.begin(), fn->modifiers.end(),
                                 "static") != fn->modifiers.end();
        std::vector<const Type*> params;
        for (auto& prm : fn->params) {
            const Type* pt = resolveTypeRepr(prm.type.get(), &rec);
            // `inout` 既可作为关键字置于形参名前，也可作为类型修饰符 `T: inout U`
            // （解析为 InoutType）。两种写法都应将形参标记为按引用传递（规范 3.1）。
            if (prm.type && prm.type->kind == NodeKind::InoutType) prm.isInout = true;
            if (prm.isInout && pt->kind != TypeKind::Ref)
                pt = types_.ref(RefKind::Mut, pt);
            else if (prm.isVariadic) pt = types_.array(pt);
            prm.semaType = pt; // authoritative for lowering
            params.push_back(pt);
        }
        const Type* ret = fn->returnType ? resolveTypeRepr(fn->returnType.get(), &rec)
                                         : types_.voidType();
        if (fn->returnType) fn->returnType->semaType = ret;
        mem.type = types_.function(std::move(params), ret);
        if (rec.isProtocol()) {
            // A default implementation (a body present) is recorded on the
            // protocol so conforming types can inherit it; a pure requirement
            // (no body) is only a requirement to be satisfied elsewhere.
            fn->isDefaultImpl = !fn->body.empty();
            rec.requirements.push_back(mem);
        } else {
            rec.members.push_back(std::move(mem));
        }
    } else if (m->kind == NodeKind::InitDecl) {
        auto* init = static_cast<InitDecl*>(m);
        TypeRecord::Member mem;
        mem.name = "init";
        mem.isFunction = true;
        mem.decl = m;
        std::vector<const Type*> params;
        for (auto& prm : init->params) {
            const Type* pt = resolveTypeRepr(prm.type.get(), &rec);
            if (prm.type && prm.type->kind == NodeKind::InoutType) prm.isInout = true;
            if (prm.isInout && pt->kind != TypeKind::Ref)
                pt = types_.ref(RefKind::Mut, pt);
            else if (prm.isVariadic) pt = types_.array(pt);
            prm.semaType = pt;
            params.push_back(pt);
        }
        mem.type = types_.function(std::move(params), types_.named(&rec, rec.name));
        rec.members.push_back(std::move(mem));
    } else if (m->kind == NodeKind::SubscriptDecl) {
        // 自定义下标（规范 3.1）：作为成员登记，get 返回元素类型、接受下标参数。
        auto* sub = static_cast<SubscriptDecl*>(m);
        TypeRecord::Member mem;
        mem.name = "subscript";
        mem.isFunction = true;
        mem.decl = m;
        const Type* idxT = sub->params.empty()
            ? types_.intType()
            : resolveTypeRepr(sub->params[0].type.get(), &rec);
        const Type* elemT = sub->elementType
            ? resolveTypeRepr(sub->elementType.get(), &rec)
            : types_.unknownType();
        mem.type = types_.function({idxT}, elemT);
        rec.members.push_back(std::move(mem));
    } else if (m->kind == NodeKind::DeinitDecl) {
        TypeRecord::Member mem;
        mem.name = "deinit"; mem.isFunction = true; mem.decl = m;
        mem.type = types_.function({}, types_.voidType());
        rec.members.push_back(std::move(mem));
    } else if (m->kind == NodeKind::EnumCaseDecl) {
        auto* ec = static_cast<EnumCaseDecl*>(m);
        TypeRecord::EnumCaseInfo ci;
        ci.name = ec->name;
        for (auto& at : ec->associatedTypes) {
            ci.associated.push_back(resolveTypeRepr(at.get(), &rec));
        }
        rec.cases.push_back(std::move(ci));
        // Enum cases are also accessible as static members `Type.case`.
        Symbol s; s.kind = Symbol::Kind::EnumCase; s.record = &rec; s.decl = m;
        globals_.declare(rec.name + "." + ec->name, s);
    } else if (m->kind == NodeKind::AssociatedTypeDecl) {
        auto* at = static_cast<AssociatedTypeDecl*>(m);
        rec.associatedTypes.push_back(at->name);
    }
}

void Sema::collectMembers(TypeRecord& rec, TypeDecl* td) {
    for (auto& m : td->members) addMember(rec, m.get());
}

// Extension members are folded into the extended type's member list so that
// lookups and code generation see one flat surface (规范 2.6). The AST of the
// extension keeps owning each member; only a pointer is shared here, so no
// ownership has to move.
void Sema::mergeExtensions(const NodeList& decls) {
    for (auto& d : decls) {
        if (!d || d->kind != NodeKind::ExtensionDecl) continue;
        auto* ext = static_cast<ExtensionDecl*>(d.get());
        // Use `findType` (not `typeIndex_[name]`) so an unknown extended type
        // such as a primitive (`extension Int`) is NOT inserted as a null
        // entry — a later pass iterates `typeIndex_` and would dereference it.
        TypeRecord* target = const_cast<TypeRecord*>(findType(ext->name));
        if (!target || !target->decl) continue;
        auto* td = static_cast<TypeDecl*>(target->decl);
        // A protocol listed in the extension's inheritance is a conformance.
        for (auto& base : ext->inherited) {
            if (!base || base->kind != NodeKind::NamedType) continue;
            const TypeRecord* br = findType(static_cast<NamedType*>(base.get())->name);
            if (br && br->isProtocol() &&
                std::find(target->protocols.begin(), target->protocols.end(),
                          br) == target->protocols.end())
                target->protocols.push_back(br);
        }
        for (auto& m : ext->members) {
            if (!m) continue;
            td->members.push_back(std::move(m)); // ownership moves to the type
        }
        // Re-collect so record->members mirrors td->members (incl. extensions).
        target->members.clear();
        target->cases.clear();
        collectMembers(*target, td);
    }
}

// A default implementation declared on a protocol is inherited by every type
// that conforms to it, unless the type already provides that member (规范 4.5).
void Sema::mergeDefaultImplementations() {
    for (auto& kv : typeIndex_) {
        TypeRecord* rec = kv.second;
        if (!rec->decl || rec->isProtocol()) continue;
        auto* td = static_cast<TypeDecl*>(rec->decl);
        for (const TypeRecord* proto : rec->protocols) {
            if (!proto) continue;
            for (auto& req : proto->requirements) {
                if (!req.isFunction || !req.decl) continue;
                auto* fd = static_cast<FunctionDecl*>(req.decl);
                if (fd->body.empty()) continue;           // pure requirement
                if (lookupMember(rec, req.name, false)) continue; // type has it
                addMember(*rec, fd); // share the declaration; proto owns it
            }
        }
    }
}

// `typealias ID = Int` — remember what the name stands for. An alias is
// transparent rather than a distinct type, so it is expanded during name
// resolution and never reaches lowering. Aliases are recorded in declaration
// order, so one may refer to types (or other aliases) declared before it.
void Sema::collectTypeAlias(TypealiasDecl* ta) {
    if (!ta || ta->name.empty() || !ta->underlying) return;
    const Type* target = resolveTypeRepr(ta->underlying.get(), nullptr);
    typeAliases_[ta->name] = target;
    if (ta->underlying) ta->underlying->semaType = target;
}

void Sema::checkOverrides() {
    // 规范 4.3 / 10.1：`override` 必须命中父类成员；`final` 成员禁止被重写。
    for (auto& kv : typeIndex_) {
        TypeRecord* rec = kv.second;
        if (!rec->decl || rec->kind != TypeDeclKind::Class || !rec->superclass)
            continue;
        auto* td = static_cast<TypeDecl*>(rec->decl);
        for (auto& m : td->members) {
            if (!m) continue;
            std::string name;
            bool isOverride = false;
            bool wantFn = false;
            if (m->kind == NodeKind::FunctionDecl) {
                auto* fn = static_cast<FunctionDecl*>(m.get());
                name = fn->name; wantFn = true;
                isOverride = std::find(fn->modifiers.begin(), fn->modifiers.end(),
                                      "override") != fn->modifiers.end();
            } else if (m->kind == NodeKind::VarDecl) {
                auto* vd = static_cast<VarDecl*>(m.get());
                name = vd->name; wantFn = false;
                isOverride = std::find(vd->modifiers.begin(), vd->modifiers.end(),
                                      "override") != vd->modifiers.end();
            } else if (m->kind == NodeKind::InitDecl) {
                auto* id = static_cast<InitDecl*>(m.get());
                name = "init"; wantFn = true;
                isOverride = std::find(id->modifiers.begin(), id->modifiers.end(),
                                      "override") != id->modifiers.end();
            }
            if (!isOverride) continue;
            // 在父类链中寻找同名同形态成员。
            const TypeRecord::Member* inherited = nullptr;
            for (const TypeRecord* r = rec->superclass; r; r = r->superclass)
                if (auto* im = lookupMember(r, name, wantFn)) { inherited = im; break; }
            if (!inherited) {
                hadError_ = true;
                diags_.reportError("'override' member '" + name +
                    "' does not override any member from a superclass",
                    rangeOf(m.get()));
                continue;
            }
            if (inherited->decl) {
                bool isFinal = false;
                if (inherited->decl->kind == NodeKind::FunctionDecl) {
                    auto* f = static_cast<FunctionDecl*>(inherited->decl);
                    isFinal = std::find(f->modifiers.begin(), f->modifiers.end(),
                                       "final") != f->modifiers.end();
                } else if (inherited->decl->kind == NodeKind::VarDecl) {
                    auto* v = static_cast<VarDecl*>(inherited->decl);
                    isFinal = std::find(v->modifiers.begin(), v->modifiers.end(),
                                       "final") != v->modifiers.end();
                }
                if (isFinal) {
                    hadError_ = true;
                    diags_.reportError("'override' member '" + name +
                        "' cannot override a 'final' member", rangeOf(m.get()));
                }
            }
        }
    }
}

void Sema::collectFunction(FunctionDecl* fn, const TypeRecord* owner) {
    Symbol s;
    s.kind = Symbol::Kind::Function;
    s.function = fn;
    s.decl = fn;
    std::vector<const Type*> params;
    TypeContext& tc = types_;
    for (auto& prm : fn->params) {
        const Type* pt = resolveTypeRepr(prm.type.get(), owner);
        // `inout` 作为类型修饰符 `T: inout U`（InoutType）时，形参按引用传递
        // （规范 3.1）。resolveTypeRepr 已将其解析为 Ref(Mut, U)。
        if (prm.type && prm.type->kind == NodeKind::InoutType) prm.isInout = true;
        if (prm.isInout && pt->kind != TypeKind::Ref)
            pt = types_.ref(RefKind::Mut, pt);
        else if (prm.isVariadic) pt = tc.array(pt);
        prm.semaType = pt; // authoritative for lowering
        params.push_back(pt);
    }
    const Type* ret = fn->returnType ? resolveTypeRepr(fn->returnType.get(), owner)
                                     : tc.voidType();
    // Record the resolved return type on the node for the code generator.
    if (fn->returnType) fn->returnType->semaType = ret;
    s.type = tc.function(std::move(params), ret);
    globals_.declare(fn->name, s);
}

void Sema::collectGlobalVar(VarDecl* vd) {
    Symbol s;
    s.kind = Symbol::Kind::Variable;
    s.isLet = vd->isLet;
    s.isVar = !vd->isLet;
    s.isWeak = vd->isWeak;
    s.decl = vd;
    globals_.declare(vd->name, s);
}

// ─── Type resolution (TypeRepr → Type) ─────────────────────────────────────
const Type* Sema::resolveTypeReprNoContext(Node* repr) {
    return resolveTypeRepr(repr, nullptr);
}

const Type* Sema::resolveTypeRepr(Node* repr, const TypeRecord* context) {
    if (!repr) return types_.unknownType();
    // Record the resolved type on the node: the code generator asks for a
    // declaration's type independently of that declaration's own semaType, and
    // the node is always available. The value is recomputed each time because a
    // generic body resolves differently per instantiation.
    repr->semaType = resolveTypeReprUncached(repr, context);
    return repr->semaType;
}

const Type* Sema::resolveTypeReprUncached(Node* repr, const TypeRecord* context) {
    if (!repr) return types_.unknownType();
    switch (repr->kind) {
        case NodeKind::NamedType: {
            auto* nt = static_cast<NamedType*>(repr);
            const std::string& name = nt->name;
            // A type parameter of the instantiation being checked stands for
            // the concrete type supplied at the call site. This must precede the
            // builtin and named lookups, otherwise `T` would never resolve.
            if (auto bt = genericBindings_.find(name);
                bt != genericBindings_.end() && bt->second)
                return bt->second;
            // A `typealias` names an existing type rather than a new one.
            if (auto ait = typeAliases_.find(name); ait != typeAliases_.end())
                return ait->second;
            TypeKind bk;
            if (builtinTypeFromName(name, bk)) {
                // Generic stdlib names
                if (name == "Array") {
                    const Type* e = nt->genericArgs.empty()
                        ? types_.unknownType()
                        : resolveTypeRepr(nt->genericArgs[0].get(), context);
                    return types_.array(e);
                }
                if (name == "Set") {
                    const Type* e = nt->genericArgs.empty()
                        ? types_.unknownType()
                        : resolveTypeRepr(nt->genericArgs[0].get(), context);
                    return types_.set(e);
                }
                if (name == "Dictionary") {
                    const Type* k = nt->genericArgs.size() > 0
                        ? resolveTypeRepr(nt->genericArgs[0].get(), context)
                        : types_.unknownType();
                    const Type* v = nt->genericArgs.size() > 1
                        ? resolveTypeRepr(nt->genericArgs[1].get(), context)
                        : types_.unknownType();
                    return types_.dict(k, v);
                }
                if (name == "Optional") {
                    const Type* e = nt->genericArgs.empty()
                        ? types_.unknownType()
                        : resolveTypeRepr(nt->genericArgs[0].get(), context);
                    return types_.optional(e);
                }
                if (name == "Owned" || name == "Unmanaged") {
                    const Type* e = nt->genericArgs.empty()
                        ? types_.unknownType()
                        : resolveTypeRepr(nt->genericArgs[0].get(), context);
                    return types_.ref(RefKind::Owned, e);
                }
                return types_.primitive(bk);
            }
            // A type parameter resolves to whatever concrete type the current
            // instantiation supplies; when none is bound (the generic body is
            // being checked on its own) it stays opaque.
            if (auto bt = genericBindings_.find(name);
                bt != genericBindings_.end() && bt->second)
                return bt->second;
            const TypeRecord* rec = findType(name);
            if (rec) {
                std::vector<const Type*> args;
                for (auto& a : nt->genericArgs) args.push_back(resolveTypeRepr(a.get(), context));
                return types_.named(rec, name, std::move(args));
            }
            return types_.unknownType();
        }
        case NodeKind::OptionalType:
            return types_.optional(resolveTypeRepr(
                static_cast<OptionalType*>(repr)->wrapped.get(), context));
        case NodeKind::ArrayType:
            return types_.array(resolveTypeRepr(
                static_cast<ArrayType*>(repr)->element.get(), context));
        case NodeKind::DictType: {
            auto* d = static_cast<DictType*>(repr);
            return types_.dict(resolveTypeRepr(d->key.get(), context),
                               resolveTypeRepr(d->value.get(), context));
        }
        case NodeKind::TupleType: {
            auto* t = static_cast<TupleType*>(repr);
            std::vector<const Type*> elems;
            for (auto& e : t->elements) elems.push_back(resolveTypeRepr(e.get(), context));
            return types_.tuple(std::move(elems), t->labels);
        }
        case NodeKind::FuncType: {
            auto* f = static_cast<FuncType*>(repr);
            std::vector<const Type*> params;
            for (auto& e : f->params) params.push_back(resolveTypeRepr(e.get(), context));
            return types_.function(std::move(params), resolveTypeRepr(f->ret.get(), context));
        }
        case NodeKind::RefType: {
            auto* r = static_cast<RefType*>(repr);
            RefKind rk = RefKind::Shared;
            if (r->refKind == "mut") rk = RefKind::Mut;
            else if (r->refKind == "weak") rk = RefKind::Weak;
            else if (r->refKind == "unowned") rk = RefKind::Unowned;
            else if (r->refKind == "owned") rk = RefKind::Owned;
            return types_.ref(rk, resolveTypeRepr(r->pointee.get(), context));
        }
        case NodeKind::InoutType:
            return types_.ref(RefKind::Mut,
                resolveTypeRepr(static_cast<InoutType*>(repr)->pointee.get(), context));
        case NodeKind::NeverType:
            return types_.neverType();
        case NodeKind::PlaceholderType:
            return types_.unknownType();
        case NodeKind::MetatypeType: {
            auto* m = static_cast<MetatypeType*>(repr);
            return types_.metatype(resolveTypeRepr(m->base.get(), context), !m->isMeta);
        }
        default:
            return types_.unknownType();
    }
}

// ─── Assignability ─────────────────────────────────────────────────────────
bool Sema::isWideningNumeric(const Type* to, const Type* from) const {
    if (!to || !from) return false;
    auto floatRank = [](TypeKind k) -> int {
        switch (k) {
            case TypeKind::Float16: return 100;
            case TypeKind::Float32: return 101;
            case TypeKind::Float64: return 102;
            case TypeKind::Float128: return 103;
            default: return 0;
        }
    };
    auto intWidth = [](TypeKind k) -> int {
        switch (k) {
            case TypeKind::Int8: case TypeKind::UInt8: return 8;
            case TypeKind::Int16: case TypeKind::UInt16: return 16;
            case TypeKind::Int32: case TypeKind::UInt32: return 32;
            case TypeKind::Int64: case TypeKind::UInt64:
            case TypeKind::Int: case TypeKind::UInt:
            case TypeKind::ISize: case TypeKind::USize: return 64;
            default: return 0;
        }
    };
    auto isSigned = [](TypeKind k) -> bool {
        return k == TypeKind::Int8 || k == TypeKind::Int16 || k == TypeKind::Int32 ||
               k == TypeKind::Int64 || k == TypeKind::Int || k == TypeKind::ISize;
    };
    const int toF = floatRank(to->kind), fromF = floatRank(from->kind);
    const int toI = intWidth(to->kind), fromI = intWidth(from->kind);
    if (toF && fromF) return toF >= fromF;          // float -> float
    if (toF && fromI) return true;                 // integer -> float
    if (toI && fromI)                              // integer -> integer
        return isSigned(to->kind) == isSigned(from->kind) && toI >= fromI;
    return false;
}

bool Sema::isAssignable(const Type* to, const Type* from) {
    if (!to || !from) return true;
    if (to->kind == TypeKind::Unknown || from->kind == TypeKind::Unknown) return true;
    if (to->kind == TypeKind::Any || to->kind == TypeKind::AnyObject) return true;
    if (from->kind == TypeKind::Never) return true;
    if (isIdentical(to, from)) return true;

    // T -> T?
    if (to->kind == TypeKind::Optional && from->kind != TypeKind::Optional)
        return isAssignable(to->element, from);
    // nil-ish: T? -> T? handled above; a bare Optional to Optional of supertype
    if (to->kind == TypeKind::Optional && from->kind == TypeKind::Optional)
        return isAssignable(to->element, from->element);

    // Collections
    if (to->kind == TypeKind::Array && from->kind == TypeKind::Array)
        return isAssignable(to->element, from->element);
    if (to->kind == TypeKind::Set && from->kind == TypeKind::Set)
        return isAssignable(to->element, from->element);
    if (to->kind == TypeKind::Dict && from->kind == TypeKind::Dict)
        return isAssignable(to->key, from->key) && isAssignable(to->value, from->value);

    // Tuples
    if (to->kind == TypeKind::Tuple && from->kind == TypeKind::Tuple) {
        if (to->elements.size() != from->elements.size()) return false;
        for (size_t i = 0; i < to->elements.size(); ++i)
            if (!isAssignable(to->elements[i], from->elements[i])) return false;
        return true;
    }

    // Functions / closures: same arity + compatible return.
    auto isFn = [](const Type* t) {
        return t->kind == TypeKind::Function || t->kind == TypeKind::Closure;
    };
    if (isFn(to) && isFn(from)) {
        if (to->elements.size() != from->elements.size()) return false;
        for (size_t i = 0; i < to->elements.size(); ++i)
            if (!isAssignable(from->elements[i], to->elements[i])) return false; // contravariant
        return isAssignable(to->ret, from->ret);
    }

    // Numeric widening
    if (isWideningNumeric(to, from)) return true;

    // Named subtyping: class -> superclass, T -> protocol it conforms to.
    if (to->kind == TypeKind::Named && from->kind == TypeKind::Named) {
        const TypeRecord* fr = from->record;
        const TypeRecord* tr = to->record;
        if (fr && tr) {
            // walk superclass chain
            for (const TypeRecord* r = fr; r; r = r->superclass) {
                if (r == tr) return true;
                for (const TypeRecord* p : r->protocols)
                    if (p == tr) return true;
            }
        }
    }

    // Metatype / Ref erasure
    if (to->kind == TypeKind::Ref && from->kind == TypeKind::Ref)
        return to->refKind == from->refKind && isAssignable(to->element, from->element);
    if (to->kind == TypeKind::Metatype && from->kind == TypeKind::Metatype)
        return isAssignable(to->element, from->element);

    return false;
}

static bool isFloatType(const Type* t) {
    if (!t) return false;
    switch (t->kind) {
        case TypeKind::Float16: case TypeKind::Float32:
        case TypeKind::Float64: case TypeKind::Float128:
            return true;
        default: return false;
    }
}

void Sema::requireAssignable(const Type* to, const Type* from, Node* at,
                             const std::string& contextDesc) {
    if (isAssignable(to, from)) return;
    // Numeric literals adapt to the contextual type: an integer literal may
    // become any integer/float type, a float literal any float type.
    if (at && to) {
        if (at->kind == NodeKind::IntLitExpr && isNumeric(to)) return;
        if (at->kind == NodeKind::FloatLitExpr && isFloatType(to)) return;
    }
    hadError_ = true;
    diags_.reportError(contextDesc + ": cannot convert value of type '" +
                       typeToString(from) + "' to '" + typeToString(to) + "'",
                       rangeOf(at));
}

// ─── Ownership ─────────────────────────────────────────────────────────────
void Sema::markMoved(Node* nameExpr, const std::string& name) {
    if (const Symbol* cs = locals_.lookup(name)) {
        const_cast<Symbol*>(cs)->moved = true;
        return;
    }
    if (const Symbol* cs = globals_.lookup(name)) {
        const_cast<Symbol*>(cs)->moved = true;
        return;
    }
    (void)nameExpr;
}

void Sema::checkNotMoved(Node* nameExpr, const std::string& name) {
    const Symbol* s = locals_.lookup(name);
    if (!s) s = globals_.lookup(name);
    if (s && s->moved) {
        hadError_ = true;
        diags_.reportError("use of moved value '" + name + "'", rangeOf(nameExpr));
    }
}

// ─── Members & call resolution ─────────────────────────────────────────────
const TypeRecord::Member* Sema::lookupMethod(const TypeRecord* rec, const std::string& name,
                                             size_t arity) const {
    if (!rec) return nullptr;
    const TypeRecord::Member* fallback = nullptr;
    for (const auto& m : rec->members) {
        if (m.isFunction && m.name == name) {
            if (m.type && m.type->elements.size() == arity) return &m;
            if (!fallback) fallback = &m;
        }
    }
    return fallback;
}

const TypeRecord::Member* Sema::lookupMember(const TypeRecord* rec, const std::string& name,
                                             bool wantFunction) const {
    if (!rec) return nullptr;
    for (const auto& m : rec->members) {
        if (m.name == name && m.isFunction == wantFunction) return &m;
    }
    return nullptr;
}

bool Sema::conformsTo(const TypeRecord* rec, const TypeRecord* proto) const {
    if (!rec || !proto) return false;
    if (rec == proto) return true;
    for (const TypeRecord* r = rec; r; r = r->superclass) {
        for (const TypeRecord* p : r->protocols) if (p == proto) return true;
    }
    return false;
}

// True when `name` is a plain decimal index, as used by tuple element access.
static bool parseTupleIndex(const std::string& name, size_t& out) {
    if (name.empty()) return false;
    size_t v = 0;
    for (char c : name) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<size_t>(c - '0');
    }
    out = v;
    return true;
}

const Type* Sema::resolveMemberType(const Type* t, const std::string& name, Node* at,
                                    const TypeRecord* context) {
    (void)context;
    if (!t) return types_.unknownType();
    if (t->kind == TypeKind::Optional) {
        // Implicit optional promotion for member access.
        return resolveMemberType(t->element, name, at, context);
    }
    if (t->kind == TypeKind::Named && t->record) {
        const TypeRecord::Member* m = lookupMethod(t->record, name, 0);
        if (m && m->isFunction) return m->type;
        m = lookupMember(t->record, name, false);
        if (m) return m->type;
        // Enum case access: `Type.case`
        for (const auto& c : t->record->cases) {
            if (c.name == name) return types_.named(t->record, c.name);
        }
        return types_.unknownType();
    }
    // Floating-point values carry the arithmetic helpers every numeric type
    // shares (规范 12.1): `squared()` is `self * self` and `squareRoot()` is
    // the IEEE-754 square root.
    if (isFloatType(t)) {
        if (name == "squareRoot" || name == "squared" || name == "abs")
            return types_.function({}, t);
        if (name == "isNaN" || name == "isInfinite")
            return types_.function({}, types_.boolType());
    }
    // Integers get the same arithmetic helpers, so a numeric generic body can
    // call them without a constraint.
    if (t && (t->kind == TypeKind::Int || t->kind == TypeKind::UInt ||
              t->kind == TypeKind::Int8 || t->kind == TypeKind::Int16 ||
              t->kind == TypeKind::Int32 || t->kind == TypeKind::Int64 ||
              t->kind == TypeKind::UInt8 || t->kind == TypeKind::UInt16 ||
              t->kind == TypeKind::UInt32 || t->kind == TypeKind::UInt64)) {
        if (name == "squared") return types_.function({}, t);
        if (name == "abs") return types_.function({}, t);
    }
    if (t->kind == TypeKind::String) {
        // `count` counts Unicode scalars, `length` counts UTF-8 bytes; both
        // are backed by the runtime string ABI.
        if (name == "count") return types_.intType();
        if (name == "length") return types_.intType();
        if (name == "isEmpty") return types_.boolType();
        if (name == "hasPrefix" || name == "hasSuffix" || name == "contains")
            return types_.function({ types_.stringType() }, types_.boolType());
        if (name == "uppercased" || name == "lowercased")
            return types_.function({}, types_.stringType());
    }
    if (t->kind == TypeKind::Array) {
        const Type* elem = t->element ? t->element : types_.unknownType();
        if (name == "count") return types_.intType();
        if (name == "isEmpty") return types_.boolType();
        if (name == "first" || name == "last") return types_.optional(elem);
        // `append` yields the updated array so `a = a.append(x)` works under
        // value semantics (the runtime may have to grow the buffer).
        if (name == "append") return types_.function({ elem }, t);
        if (name == "removeAt" || name == "removeAll")
            return types_.function({ types_.intType() }, t);
        if (name == "contains")
            return types_.function({ elem }, types_.boolType());
    }
    if (t->kind == TypeKind::Dict) {
        const Type* k = t->key ? t->key : types_.unknownType();
        const Type* v = t->value ? t->value : types_.unknownType();
        if (name == "count") return types_.intType();
        if (name == "isEmpty") return types_.boolType();
        // Subscript assignment `d[k] = v` desugars to these two, so they take
        // and return the value type rather than a pointer.
        if (name == "set" || name == "update")
            return types_.function({ k, v }, types_.voidType());
        if (name == "get") return types_.function({ k }, types_.optional(v));
        if (name == "removeValue") return types_.function({ k }, types_.optional(v));
        if (name == "containsKey") return types_.function({ k }, types_.boolType());
        if (name == "keys" || name == "values")
            return types_.function({}, types_.array(k));
    }
    if (t->kind == TypeKind::Set) {
        const Type* e = t->element ? t->element : types_.unknownType();
        if (name == "count") return types_.intType();
        if (name == "isEmpty") return types_.boolType();
        // A set is a dictionary whose values are unit-sized; the runtime
        // implements it with val_size == 0 and ignores the value entirely.
        if (name == "insert" || name == "remove")
            return types_.function({ e }, types_.boolType());
        if (name == "contains") return types_.function({ e }, types_.boolType());
    }
    // Unknown member → report once via Unknown so we do not cascade.
    return types_.unknownType();
}

const Type* Sema::resolveCallType(const Type* calleeType, Node* at,
                                  const TypeRecord* context, size_t argCount) {
    (void)at; (void)context;
    if (!calleeType) return types_.unknownType();
    if (calleeType->kind == TypeKind::Function || calleeType->kind == TypeKind::Closure) {
        (void)argCount;
        return calleeType->ret ? calleeType->ret : types_.unknownType();
    }
    if (calleeType->kind == TypeKind::Optional && calleeType->element &&
        (calleeType->element->kind == TypeKind::Function ||
         calleeType->element->kind == TypeKind::Closure)) {
        return calleeType->element->ret ? calleeType->element->ret : types_.unknownType();
    }
    return types_.unknownType();
}

// ─── Unresolved name helper ────────────────────────────────────────────────
const Type* Sema::reportUnresolved(Node* identExpr, const std::string& name) {
    hadError_ = true;
    diags_.reportError("cannot find '" + name + "' in scope", rangeOf(identExpr));
    return types_.unknownType();
}

// ─── Function / global bodies ──────────────────────────────────────────────
void Sema::checkFunctionBody(FunctionDecl* fn, const TypeRecord* owner) {
    if (fn->isForeign) return; // external declaration: no body to check
    currentType_ = owner;
    currentThrows_ = fn->isThrows;
    const Type* ret = fn->returnType ? resolveTypeRepr(fn->returnType.get(), owner)
                                     : types_.voidType();
    currentReturn_ = ret;

    locals_.pushScope();
    std::vector<std::string> savedGeneric = genericParams_;
    genericParams_ = fn->genericParams;

    if (owner) {
        Symbol self;
        self.kind = Symbol::Kind::Variable;
        self.type = types_.named(owner, owner->name);
        self.decl = fn;
        locals_.declare("self", self);
    }
    for (auto& prm : fn->params) {
        Symbol s;
        s.kind = Symbol::Kind::Parameter;
        s.type = resolveTypeRepr(prm.type.get(), owner);
        if (prm.isVariadic) s.type = types_.array(s.type);
        s.decl = fn;
        locals_.declare(prm.internalName, s);
        if (prm.defaultValue) {
            const Type* dt = checkExpr(prm.defaultValue.get(), owner);
            requireAssignable(s.type, dt, prm.defaultValue.get(), "default argument");
        }
    }

    checkStatements(fn->body, owner, ret, fn->isThrows);

    // Every non-Void function must return on all paths. Protocol requirements
    // and other declaration-only members have an empty body and are exempt.
    if (ret && ret->kind != TypeKind::Void && ret->kind != TypeKind::Unknown &&
        !fn->body.empty() && !blockAlwaysTransfers(fn->body)) {
        hadError_ = true;
        diags_.reportError("missing return in function '" + fn->name +
                           "' (expected '" + typeToString(ret) + "')", rangeOf(fn));
    }

    genericParams_ = savedGeneric;
    locals_.popScope();
    currentType_ = nullptr;
    currentReturn_ = nullptr;
    currentThrows_ = false;
}

void Sema::checkGlobalVarBody(VarDecl* vd) {
    const Type* declared = vd->type ? resolveTypeRepr(vd->type.get(), nullptr) : nullptr;
    // `let c: Char = "A"` —— 单标量字符串字面量在 Char 上下文里是一个字符，
    // 而不是 String（规范 1.5）。
    const Type* init = nullptr;
    if (vd->initializer) {
        const Type* dt = declared ? declared : nullptr;
        init = (dt && dt->kind == TypeKind::Char)
            ? checkAsChar(vd->initializer.get(), nullptr)
            : checkExpr(vd->initializer.get(), nullptr);
    }
    if (declared && init)
        requireAssignable(declared, init, vd->initializer.get(), "variable initializer");
    if (!declared && init) {
        if (const Symbol* cs = globals_.lookup(vd->name))
            const_cast<Symbol*>(cs)->type = init;
    }
    // 顶层 `let x: Int` 同样允许延迟初始化。
    if (!vd->name.empty() && !vd->initializer && declared)
        pendingInit_.insert(vd->name);
}

// ─── Statements ────────────────────────────────────────────────────────────
void Sema::checkStatements(const NodeList& stmts, const TypeRecord* context,
                           const Type* fnReturnType, bool isThrowing) {
    for (auto& s : stmts) checkStatement(s.get(), context, fnReturnType, isThrowing);
}

// Declare a binding introduced by a condition / for-in pattern.
static const Type* declareLocal(Sema& /*unused*/, ScopedTable<Symbol>& locals,
                                const std::string& name, const Type* type, Node* decl,
                                bool isLet) {
    Symbol s;
    s.kind = Symbol::Kind::Variable;
    s.type = type;
    s.isLet = isLet;
    s.decl = decl;
    locals.declare(name, s);
    return type;
}

void Sema::checkStatement(Node* stmt, const TypeRecord* context,
                          const Type* fnReturnType, bool isThrowing) {
    if (!stmt) return;
    switch (stmt->kind) {
        case NodeKind::VarDecl: {
            auto* vd = static_cast<VarDecl*>(stmt);
            const Type* declared = vd->type ? resolveTypeRepr(vd->type.get(), context) : nullptr;
            // 元组解构：`let (a, b) = (1, 2)`。初始化器必须是元组，各绑定名
            // 依次取其分量类型；个数不匹配是错误。
            if (!vd->tupleNames.empty()) {
                if (!vd->initializer) {
                    hadError_ = true;
                    diags_.reportError("tuple pattern '" + vd->name +
                                           "' needs an initialiser",
                                       rangeOf(vd));
                } else {
                    const Type* it = checkExpr(vd->initializer.get(), context);
                    if (it && it->kind == TypeKind::Tuple) {
                        if (it->elements.size() != vd->tupleNames.size()) {
                            hadError_ = true;
                            diags_.reportError(
                                "tuple pattern expects " +
                                    std::to_string(vd->tupleNames.size()) +
                                    " values, found " +
                                    std::to_string(it->elements.size()),
                                rangeOf(vd));
                        }
                        for (size_t i = 0; i < vd->tupleNames.size(); ++i) {
                            const Type* et = i < it->elements.size()
                                                 ? it->elements[i]
                                                 : types_.unknownType();
                            Symbol s;
                            s.kind = Symbol::Kind::Variable;
                            s.type = et;
                            s.isLet = vd->isLet;
                            locals_.declare(vd->tupleNames[i], s);
                        }
                    } else {
                        hadError_ = true;
                        diags_.reportError(
                            "cannot destructure a value of type '" + typeToString(it) +
                                "' with a tuple pattern",
                            rangeOf(vd->initializer.get()));
                    }
                }
                vd->semaType = vd->initializer ? vd->initializer->semaType
                                              : types_.unknownType();
                break;
            }
            const Type* init = nullptr;
            if (vd->initializer) {
                const Type* dt = declared ? declared : nullptr;
                init = (dt && dt->kind == TypeKind::Char)
                    ? checkAsChar(vd->initializer.get(), context)
                    : checkExpr(vd->initializer.get(), context);
                // `let s: Set<Int> = {1, 2}` — the empty (or bare) literal has
                // nothing to infer from, so the annotation supplies the element.
                if (dt && dt->kind == TypeKind::Set &&
                    vd->initializer->kind == NodeKind::SetLitExpr)
                    vd->initializer->semaType = dt;
            }
            if (declared && init)
                requireAssignable(declared, init, vd->initializer.get(), "variable initializer");
            const Type* type = declared ? declared : (init ? init : types_.unknownType());
            vd->semaType = type; // annotated for the code generator
            // Propagate an explicit annotation onto an empty collection literal
            // (`let a: [Int] = []`, `let d: [Int: Int] = [:]`). Such a literal
            // carries no elements to infer from, so without this it would stay
            // `Array<Unknown>` / `Dictionary<Unknown, Unknown>` and lowering
            // would pick the wrong element stride.
            if (declared && vd->initializer) {
                Node* init = vd->initializer.get();
                bool emptyArray = init->kind == NodeKind::ArrayLitExpr &&
                                  static_cast<ArrayLitExpr*>(init)->elements.empty();
                bool emptyDict = init->kind == NodeKind::DictLitExpr &&
                                 static_cast<DictLitExpr*>(init)->keys.empty();
                if (!init->semaType || emptyArray || emptyDict)
                    init->semaType = declared;
            }
            // Computed property accessors, if present, are checked as a body.
            // Accessor bodies are checked in the owner's context so that
            // `self` and the owner's members resolve.
            for (auto& acc : vd->accessors) {
                if (!acc || acc->kind != NodeKind::AccessorDecl) continue;
                auto* ad = static_cast<AccessorDecl*>(acc.get());
                // A getter returns the property type; the others return Void.
                const Type* accRet = ad->kind == AccessorDecl::Kind::Getter
                    ? type : types_.voidType();
                // 访问器体内隐式 self 成员（如 `grid`）需要在属主类型上下文中解析，
                // 故临时将 currentType_ 设为属主（规范 1.7 / 3.1）。
                const TypeRecord* savedType = currentType_;
                currentType_ = context;
                checkStatements(ad->body, context, accRet, isThrowing);
                currentType_ = savedType;
            }
            declareLocal(*this, locals_, vd->name, type, vd, vd->isLet);
            // `let x: Int`（无初始化器）进入待初始化状态：首次赋值即初始化，
            // 之后不可再赋值（规范 1.3 允许延迟初始化）。
            if (!vd->name.empty() && !vd->initializer && declared)
                pendingInit_.insert(vd->name);
            break;
        }
        case NodeKind::BlockStmt: {
            // `let a = 1, b = 2` desugars to a BlockStmt of VarDecls.
            auto* b = static_cast<BlockStmt*>(stmt);
            checkStatements(b->statements, context, fnReturnType, isThrowing);
            break;
        }
        case NodeKind::ExprStmt:
            checkExpr(static_cast<ExprStmt*>(stmt)->expr.get(), context);
            break;
        case NodeKind::ReturnStmt: {
            auto* r = static_cast<ReturnStmt*>(stmt);
            if (r->value) {
                // A `Char`-returning function reads a one-scalar string literal
                // as a character.
                const Type* vt =
                    (fnReturnType && fnReturnType->kind == TypeKind::Char)
                        ? checkAsChar(r->value.get(), context)
                        : checkExpr(r->value.get(), context);
                if (fnReturnType)
                    requireAssignable(fnReturnType, vt, r->value.get(), "return");
            }
            break;
        }
        case NodeKind::IfStmt: {
            auto* ifs = static_cast<IfStmt*>(stmt);
            locals_.pushScope();
            if (ifs->condition) {
                if (ifs->condition->kind == NodeKind::VarDecl) {
                    // `if let x = opt` — declare the unwrapped binding.
                    auto* vd = static_cast<VarDecl*>(ifs->condition.get());
                    const Type* init = vd->initializer
                        ? checkExpr(vd->initializer.get(), context) : types_.unknownType();
                    const Type* bound = (init && init->kind == TypeKind::Optional)
                        ? init->element : init;
                    declareLocal(*this, locals_, vd->name, bound, vd, true);
                } else {
                    const Type* ct = checkExpr(ifs->condition.get(), context);
                    if (ct && ct->kind != TypeKind::Bool && ct->kind != TypeKind::Unknown)
                        hadError_ = true, diags_.reportError(
                            "condition must be Bool, found '" + typeToString(ct) + "'",
                            rangeOf(ifs->condition.get()));
                }
            }
            checkStatements(ifs->thenBody, context, fnReturnType, isThrowing);
            if (ifs->elseBranch) checkStatement(ifs->elseBranch.get(), context, fnReturnType, isThrowing);
            locals_.popScope();
            break;
        }
        case NodeKind::IfExpr: {
            auto* e = static_cast<IfExpr*>(stmt);
            if (e->condition && e->condition->kind != NodeKind::VarDecl) {
                const Type* ct = checkExpr(e->condition.get(), context);
                if (ct && ct->kind != TypeKind::Bool && ct->kind != TypeKind::Unknown)
                    diags_.reportError("condition must be Bool", rangeOf(e->condition.get()));
            } else if (e->condition) {
                checkStatement(e->condition.get(), context, fnReturnType, isThrowing);
            }
            checkStatements(e->thenBody, context, fnReturnType, isThrowing);
            if (e->elseBranch) checkStatement(e->elseBranch.get(), context, fnReturnType, isThrowing);
            break;
        }
        case NodeKind::GuardStmt: {
            auto* g = static_cast<GuardStmt*>(stmt);
            if (g->condition && g->condition->kind == NodeKind::VarDecl) {
                auto* vd = static_cast<VarDecl*>(g->condition.get());
                const Type* init = vd->initializer ? checkExpr(vd->initializer.get(), context)
                                                   : types_.unknownType();
                const Type* bound = (init && init->kind == TypeKind::Optional) ? init->element : init;
                declareLocal(*this, locals_, vd->name, bound, vd, true);
            } else if (g->condition) {
                const Type* ct = checkExpr(g->condition.get(), context);
                if (ct && ct->kind != TypeKind::Bool && ct->kind != TypeKind::Unknown)
                    diags_.reportError("condition must be Bool", rangeOf(g->condition.get()));
            }
            checkStatements(g->elseBody, context, fnReturnType, isThrowing);
            break;
        }
        case NodeKind::WhileStmt: {
            auto* w = static_cast<WhileStmt*>(stmt);
            if (w->condition && w->condition->kind == NodeKind::VarDecl) {
                checkStatement(w->condition.get(), context, fnReturnType, isThrowing);
            } else if (w->condition) {
                const Type* ct = checkExpr(w->condition.get(), context);
                if (ct && ct->kind != TypeKind::Bool && ct->kind != TypeKind::Unknown)
                    diags_.reportError("while condition must be Bool", rangeOf(w->condition.get()));
            }
            locals_.pushScope();
            checkStatements(w->body, context, fnReturnType, isThrowing);
            locals_.popScope();
            break;
        }
        case NodeKind::RepeatWhileStmt: {
            auto* r = static_cast<RepeatWhileStmt*>(stmt);
            locals_.pushScope();
            checkStatements(r->body, context, fnReturnType, isThrowing);
            locals_.popScope();
            if (r->condition) {
                const Type* ct = checkExpr(r->condition.get(), context);
                if (ct && ct->kind != TypeKind::Bool && ct->kind != TypeKind::Unknown)
                    diags_.reportError("while condition must be Bool", rangeOf(r->condition.get()));
            }
            break;
        }
        case NodeKind::ForInStmt: {
            auto* f = static_cast<ForInStmt*>(stmt);
            const Type* seq = checkExpr(f->sequence.get(), context);
            locals_.pushScope();
            // Element type produced by iterating `seq`.
            const Type* elem = types_.unknownType();
            // A range has no semantic type of its own yet, but iterating one
            // always yields integers (`for i in 0..<10` binds an Int), so the
            // element is fixed here rather than left unknown.
            if (f->sequence && f->sequence->kind == NodeKind::RangeExpr)
                elem = types_.intType();
            if (seq) {
                switch (seq->kind) {
                    case TypeKind::Array: case TypeKind::Set: elem = seq->element; break;
                    case TypeKind::Dict: elem = types_.tuple({seq->key, seq->value}); break;
                    case TypeKind::String: elem = types_.charType(); break;
                    case TypeKind::Optional: elem = seq->element; break;
                    case TypeKind::Named:
                        if (seq->record) {
                            for (const auto& m : seq->record->members) {
                                if (m.isFunction && m.name == "makeIterator" && m.type &&
                                    m.type->ret) { elem = m.type->ret; }
                            }
                        }
                        break;
                    default: break;
                }
            }
            // Declare the loop variable(s).
            if (f->pattern) {
                if (f->pattern->kind == NodeKind::VarDecl) {
                    auto* vd = static_cast<VarDecl*>(f->pattern.get());
                    const Type* pt = vd->type ? resolveTypeRepr(vd->type.get(), context) : elem;
                    // The binding's type must be on the node too: lowering asks
                    // for it at every use (`print(i)` needs to know `i` is an
                    // Int), not only in the symbol table.
                    vd->semaType = pt;
                    declareLocal(*this, locals_, vd->name, pt, vd, true);
                } else if (f->pattern->kind == NodeKind::TupleExpr) {
                    auto* tup = static_cast<TupleExpr*>(f->pattern.get());
                    for (size_t i = 0; i < tup->elements.size(); ++i) {
                        auto& el = tup->elements[i];
                        if (el && el->kind == NodeKind::IdentExpr) {
                            const std::string& nm = static_cast<IdentExpr*>(el.get())->name;
                            const Type* et = (elem && elem->kind == TypeKind::Tuple &&
                                              i < elem->elements.size())
                                ? elem->elements[i] : types_.unknownType();
                            declareLocal(*this, locals_, nm, et, el.get(), true);
                        }
                    }
                }
            }
            checkStatements(f->body, context, fnReturnType, isThrowing);
            locals_.popScope();
            break;
        }
        case NodeKind::SwitchStmt: {
            auto* s = static_cast<SwitchStmt*>(stmt);
            const Type* subj = s->subject ? checkExpr(s->subject.get(), context)
                                          : types_.unknownType();
            // `case let v` needs the subject's type to type the binding.
            const Type* savedSubject = switchSubjectType_;
            switchSubjectType_ = subj;
            for (auto& c : s->cases) checkStatement(c.get(), context, fnReturnType, isThrowing);
            switchSubjectType_ = savedSubject;

            // 穷举检查（规范 1.7）：枚举 switch 必须覆盖全部 case，否则必须提供
            // `default` 或值绑定（`case let v`）兜底分支。
            if (subj && subj->kind == TypeKind::Named && subj->record &&
                subj->record->kind == TypeDeclKind::Enum) {
                const TypeRecord* erec = subj->record;
                std::vector<std::string> covered;
                bool hasDefault = false, hasBinding = false;
                auto collectEnumCase = [&](Node* p) {
                    if (!p || p->kind != NodeKind::MemberExpr) return;
                    auto* pm = static_cast<MemberExpr*>(p);
                    if (pm->base && pm->base->kind == NodeKind::IdentExpr &&
                        static_cast<IdentExpr*>(pm->base.get())->name == erec->name)
                        covered.push_back(pm->member);
                };
                for (auto& c : s->cases) {
                    if (!c || c->kind != NodeKind::CaseClause) continue;
                    auto* cc = static_cast<CaseClause*>(c.get());
                    if (cc->isDefault) { hasDefault = true; continue; }
                    if (cc->isBindingPattern) { hasBinding = true; continue; }
                    collectEnumCase(cc->pattern.get());
                    for (auto& alt : cc->alternatives) collectEnumCase(alt.get());
                }
                if (!hasDefault && !hasBinding) {
                    std::string missing;
                    for (const auto& ci : erec->cases)
                        if (std::find(covered.begin(), covered.end(), ci.name) == covered.end())
                            missing += (missing.empty() ? "" : ", ") + ci.name;
                    if (!missing.empty()) {
                        hadError_ = true;
                        diags_.reportError("switch over enum '" + erec->name +
                            "' must be exhaustive; missing case(s): " + missing,
                            s->subject ? rangeOf(s->subject.get()) : rangeOf(stmt));
                    }
                }
            }
            (void)subj;
            break;
        }
        case NodeKind::CaseClause: {
            auto* c = static_cast<CaseClause*>(stmt);
            if (c->pattern) {
                inPattern_ = true;
                checkExpr(c->pattern.get(), context);
                inPattern_ = false;
            }
            for (auto& alt : c->alternatives) {
                if (!alt || alt->kind != NodeKind::CaseClause) continue;
                auto* ac = static_cast<CaseClause*>(alt.get());
                if (!ac->pattern) continue;
                inPattern_ = false;
                checkExpr(ac->pattern.get(), context);
            }
            locals_.pushScope();
            // `case let v` / `case var v` — 值绑定模式：把 subject 以其类型绑定
            // 到 v，随后 `where` 子句可用 v 过滤（规范 1.7）。
            if (c->isBindingPattern) {
                const Type* bt = switchSubjectType_ ? switchSubjectType_
                                                    : types_.unknownType();
                for (const std::string& b : c->bindings) {
                    Symbol bs;
                    bs.kind = Symbol::Kind::Variable;
                    bs.type = bt;
                    bs.isLet = true;
                    locals_.declare(b, bs);
                }
            }
            // Payload bindings introduced by the pattern (`let r`, `let w, ...`)
            // are visible only inside this arm, typed by the case declaration.
            if (!c->isBindingPattern && !c->bindings.empty() && c->pattern &&
                c->pattern->kind == NodeKind::MemberExpr) {
                auto* pm = static_cast<MemberExpr*>(c->pattern.get());
                if (pm->base && pm->base->kind == NodeKind::IdentExpr) {
                    const std::string& bn = static_cast<IdentExpr*>(pm->base.get())->name;
                    if (const TypeRecord* rec = findType(bn)) {
                        if (rec->kind == TypeDeclKind::Enum) {
                            for (auto& ci : rec->cases) {
                                if (ci.name != pm->member) continue;
                                for (size_t i = 0;
                                     i < c->bindings.size() && i < ci.associated.size(); ++i) {
                                    Symbol bs;
                                    bs.kind = Symbol::Kind::Variable;
                                    bs.type = ci.associated[i];
                                    locals_.declare(c->bindings[i], bs);
                                }
                            }
                        }
                    }
                }
            }
            // `where` is evaluated after the bindings are in scope, so it can
            // refer to them (`case let v where v > 100`).
            if (c->whereExpr) checkExpr(c->whereExpr.get(), context);
            {
                const bool savedInCase = inCaseBody_;
                inCaseBody_ = true;
                checkStatements(c->body, context, fnReturnType, isThrowing);
                inCaseBody_ = savedInCase;
            }
            locals_.popScope();
            break;
        }
        case NodeKind::ThrowStmt: {
            auto* t = static_cast<ThrowStmt*>(stmt);
            checkExpr(t->value.get(), context);
            if (!isThrowing) {
                hadError_ = true;
                diags_.reportError("error thrown from a non-throwing context",
                                   rangeOf(t->value.get()));
            }
            break;
        }
        case NodeKind::DoStmt: {
            auto* d = static_cast<DoStmt*>(stmt);
            checkStatements(d->body, context, fnReturnType, isThrowing);
            for (auto& c : d->catches) checkStatement(c.get(), context, fnReturnType, isThrowing);
            break;
        }
        case NodeKind::CatchClause: {
            auto* c = static_cast<CatchClause*>(stmt);
            locals_.pushScope();
            if (c->pattern) {
                if (c->pattern->kind == NodeKind::VarDecl) {
                    auto* vd = static_cast<VarDecl*>(c->pattern.get());
                    const Type* t = vd->type ? resolveTypeRepr(vd->type.get(), context)
                                             : types_.errorType();
                    declareLocal(*this, locals_, vd->name, t, vd, true);
                } else {
                    checkExpr(c->pattern.get(), context);
                }
            }
            if (c->whereExpr) checkExpr(c->whereExpr.get(), context);
            checkStatements(c->body, context, fnReturnType, isThrowing);
            locals_.popScope();
            break;
        }
        case NodeKind::DeferStmt:
            checkStatement(static_cast<DeferStmt*>(stmt)->body.get(), context, fnReturnType, isThrowing);
            break;
        case NodeKind::UnsafeStmt:
            checkStatements(static_cast<UnsafeStmt*>(stmt)->body, context, fnReturnType, isThrowing);
            break;
        case NodeKind::BreakStmt:
        case NodeKind::ContinueStmt:
            break;
        case NodeKind::FallthroughStmt:
            // `fallthrough` is only meaningful as the last statement of a
            // non-final switch arm (规范 1.7). Outside a switch it is an error.
            if (!inCaseBody_) {
                hadError_ = true;
                diags_.reportError("'fallthrough' is only allowed inside a switch case",
                                   rangeOf(stmt));
            }
            break;
        default:
            // Expressions used as statements (e.g. bare closures).
            checkExpr(stmt, context);
            break;
    }
}

// ─── Control flow ──────────────────────────────────────────────────────────
bool Sema::alwaysTransfers(Node* stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case NodeKind::ReturnStmt:
        case NodeKind::ThrowStmt:
        case NodeKind::BreakStmt:
        case NodeKind::ContinueStmt:
            return true;
        case NodeKind::BlockStmt:
            return blockAlwaysTransfers(static_cast<BlockStmt*>(stmt)->statements);
        case NodeKind::DoStmt: {
            // `do { return } catch { return }` transfers on every path.
            auto* d = static_cast<DoStmt*>(stmt);
            if (!blockAlwaysTransfers(d->body)) return false;
            for (auto& c : d->catches) {
                if (!blockAlwaysTransfers(static_cast<CatchClause*>(c.get())->body))
                    return false;
            }
            return true;
        }
        case NodeKind::IfStmt: {
            auto* ifs = static_cast<IfStmt*>(stmt);
            bool thenT = blockAlwaysTransfers(ifs->thenBody);
            bool elseT = ifs->elseBranch ? alwaysTransfers(ifs->elseBranch.get()) : false;
            return thenT && elseT;
        }
        case NodeKind::SwitchStmt: {
            // An exhaustive switch (has `default`) whose every arm transfers
            // control transfers on all paths.
            auto* sw = static_cast<SwitchStmt*>(stmt);
            bool hasDefault = false;
            for (auto& c : sw->cases) {
                if (!c || c->kind != NodeKind::CaseClause) continue;
                auto* cc = static_cast<CaseClause*>(c.get());
                if (cc->isDefault) hasDefault = true;
                if (!blockAlwaysTransfers(cc->body)) return false;
            }
            return hasDefault;
        }
        default:
            return false;
    }
}

bool Sema::blockAlwaysTransfers(const NodeList& stmts) {
    for (auto& s : stmts) {
        if (alwaysTransfers(s.get())) return true;
    }
    return false;
}

// ─── Expression type inference ─────────────────────────────────────────────
const Type* Sema::checkExpr(Node* e, const TypeRecord* context) {
    const Type* t = checkExprInner(e, context);
    if (e) e->semaType = t;
    return t;
}

const Type* Sema::checkExprInner(Node* e, const TypeRecord* context) {
    if (!e) return types_.unknownType();
    switch (e->kind) {
        case NodeKind::IntLitExpr:  return types_.intType();
        case NodeKind::FloatLitExpr: return types_.doubleType();
        case NodeKind::CharLitExpr:  return types_.charType();
        case NodeKind::BoolLitExpr:  return types_.boolType();
        case NodeKind::NilLitExpr:   return types_.unknownType();
        case NodeKind::TernaryExpr: {
            auto* t = static_cast<TernaryExpr*>(e);
            const Type* ct = checkExpr(t->condition.get(), context);
            if (ct && ct->kind != TypeKind::Bool && ct->kind != TypeKind::Unknown
                && ct->kind != TypeKind::Optional) {
                hadError_ = true;
                diags_.reportError(
                    "condition of '?:' must be Bool, found '" + typeToString(ct) + "'",
                    rangeOf(e));
            }
            const Type* tt = checkExpr(t->thenValue.get(), context);
            const Type* et = checkExpr(t->elseValue.get(), context);
            // Both branches must agree; a numeric widening is allowed.
            if (tt && et && tt->kind != TypeKind::Unknown &&
                et->kind != TypeKind::Unknown && tt->kind != et->kind) {
                hadError_ = true;
                diags_.reportError(
                    "branches of '?:' have incompatible types '" +
                    typeToString(tt) + "' and '" + typeToString(et) + "'",
                    rangeOf(e));
                return types_.unknownType();
            }
            return (tt && tt->kind != TypeKind::Unknown) ? tt : et;
        }
        case NodeKind::StrLitExpr: {
            auto* s = static_cast<StrLitExpr*>(e);
            for (auto& ex : s->expressions) checkExpr(ex.get(), context);
            // In a `Char` position a single-scalar string denotes a character.
            if (charContext_) {
                std::string text;
                for (const auto& seg : s->segments) text += seg;
                if (text.size() == 1) return types_.charType();
                hadError_ = true;
                diags_.reportError(
                    "character literal must contain exactly one character",
                    rangeOf(e));
                return types_.charType();
            }
            return types_.stringType();
        }
        case NodeKind::IdentExpr: {
            auto* id = static_cast<IdentExpr*>(e);
            const std::string& name = id->name;
            // `super` is only meaningful inside a class that has a superclass.
            if (name == "super") {
                if (currentType_ && currentType_->superclass) {
                    id->semaType = types_.named(currentType_->superclass,
                                               currentType_->superclass->name);
                    return id->semaType;
                }
                hadError_ = true;
                diags_.reportError(
                    "'super' used outside a class with a superclass", rangeOf(e));
                return types_.unknownType();
            }
            checkNotMoved(e, name);
            // Every reference records its resolved type on the node: lowering
            // (member access, tuple indexing, overload choice) consumes semaType
            // rather than re-deriving types from syntax.
            if (const Symbol* s = locals_.lookup(name)) {
                id->semaType = s->type ? s->type : types_.unknownType();
                return id->semaType;
            }
            if (const Symbol* s = globals_.lookup(name)) {
                id->semaType = s->type ? s->type : types_.unknownType();
                return id->semaType;
            }
            // Implicit self: inside a type's method, a bare member name refers
            // to `self.<name>` (e.g. `x` for `self.x`).
            if (currentType_) {
                if (const TypeRecord::Member* m = lookupMember(currentType_, name, false)) {
                    id->semaType = m->type ? m->type : types_.unknownType();
                    return id->semaType;
                }
                if (const TypeRecord::Member* m = lookupMember(currentType_, name, true)) {
                    id->semaType = m->type ? m->type : types_.unknownType();
                    return id->semaType;
                }
                for (const auto& c : currentType_->cases)
                    if (c.name == name) return types_.named(currentType_, c.name);
            }
            // A bare type name is a type reference (e.g. `String.self` base).
            if (findType(name)) return types_.unknownType();
            TypeKind bk;
            if (builtinTypeFromName(name, bk)) return types_.unknownType();
            // In a binding pattern an unresolved bare identifier is a new
            // binding, not an error.
            if (inPattern_) return types_.unknownType();
            return reportUnresolved(e, name);
        }
        case NodeKind::BinaryExpr: {
            auto* b = static_cast<BinaryExpr*>(e);
            const Type* lt = checkExpr(b->lhs.get(), context);
            const Type* rt = checkExpr(b->rhs.get(), context);
            // `c == "A"`: a one-scalar string literal compared with a Char is the
            // character, not a String (规范 1.5). The literal is therefore
            // re-checked in a Char context so the code generator sees an i32.
            if (lt && rt && b->lhs && b->rhs &&
                lt->kind == TypeKind::Char && rt->kind == TypeKind::String &&
                b->rhs->kind == NodeKind::StrLitExpr) {
                charContext_ = true;
                rt = checkExpr(b->rhs.get(), context);
                charContext_ = false;
            } else if (lt && rt && b->lhs && b->rhs &&
                       rt->kind == TypeKind::Char && lt->kind == TypeKind::String &&
                       b->lhs->kind == NodeKind::StrLitExpr) {
                charContext_ = true;
                lt = checkExpr(b->lhs.get(), context);
                charContext_ = false;
            }
            switch (b->op) {
                case PunctuatorID::EqualEqual: case PunctuatorID::BangEqual:
                case PunctuatorID::Less: case PunctuatorID::Greater:
                case PunctuatorID::LessEqual: case PunctuatorID::GreaterEqual:
                    return types_.boolType();
                case PunctuatorID::AmpAmp: case PunctuatorID::PipePipe: {
                    auto bad = [&](const Type* t) {
                        if (t && t->kind != TypeKind::Bool && t->kind != TypeKind::Unknown) {
                            hadError_ = true;
                            diags_.reportError("logical operator requires Bool operands, found '" +
                                               typeToString(t) + "'", rangeOf(e));
                        }
                    };
                    bad(lt); bad(rt);
                    return types_.boolType();
                }
                case PunctuatorID::QuestionQuestion: {
                    if (lt && lt->kind == TypeKind::Optional) return lt->element;
                    return lt ? lt : types_.unknownType();
                }
                case PunctuatorID::LeftArrow:
                    return types_.unknownType();
                default: {
                    // arithmetic / bitwise
                    bool ok = (isNumeric(lt) || !lt || lt->kind == TypeKind::Unknown) &&
                              (isNumeric(rt) || !rt || rt->kind == TypeKind::Unknown);
                    if (!ok && lt && lt->kind == TypeKind::String &&
                        (b->op == PunctuatorID::Plus))
                        ok = (rt && rt->kind == TypeKind::String);
                    if (!ok) {
                        hadError_ = true;
                        diags_.reportError("invalid operands to binary operator '" +
                                           std::string(punctToString(b->op)) +
                                           "' ('" + typeToString(lt) + "' and '" +
                                           typeToString(rt) + "')", rangeOf(e));
                    }
                    return lt ? lt : types_.unknownType();
                }
            }
        }
        case NodeKind::UnaryExpr: {
            auto* u = static_cast<UnaryExpr*>(e);
            const Type* t = checkExpr(u->operand.get(), context);
            // `&x` 取地址：产生 `inout T`，供按引用传参（规范 3.1）。
            if (u->op == PunctuatorID::Amp) {
                if (t && t->kind != TypeKind::Unknown)
                    return types_.ref(RefKind::Mut, t);
                return types_.unknownType();
            }
            // `try?` wraps the result in an Optional (规范 9.2): an thrown error
            // becomes `nil` instead of propagating.
            if (u->isOptionalTry && t && t->kind != TypeKind::Unknown)
                return types_.optional(t);
            // `try` and `await` reuse markers; they pass the operand type through.
            return t ? t : types_.unknownType();
        }
        case NodeKind::MoveExpr: {
            auto* m = static_cast<MoveExpr*>(e);
            const Type* t = checkExpr(m->operand.get(), context);
            if (m->operand && m->operand->kind == NodeKind::IdentExpr)
                markMoved(e, static_cast<IdentExpr*>(m->operand.get())->name);
            return t ? t : types_.unknownType();
        }
        case NodeKind::ParenExpr:
            return checkExpr(static_cast<ParenExpr*>(e)->expr.get(), context);
        case NodeKind::ForceUnwrapExpr: {
            const Type* t = checkExpr(static_cast<ForceUnwrapExpr*>(e)->expr.get(), context);
            if (t && t->kind == TypeKind::Optional) return t->element;
            return t ? t : types_.unknownType();
        }
        case NodeKind::OptionalChainExpr: {
            const Type* t = checkExpr(static_cast<OptionalChainExpr*>(e)->expr.get(), context);
            return types_.optional(t);
        }
        case NodeKind::TupleExpr: {
            auto* tup = static_cast<TupleExpr*>(e);
            std::vector<const Type*> elems;
            for (auto& el : tup->elements) elems.push_back(checkExpr(el.get(), context));
            return types_.tuple(std::move(elems), tup->labels);
        }
        case NodeKind::SetLitExpr: {
            auto* sl = static_cast<SetLitExpr*>(e);
            const Type* elem = types_.unknownType();
            bool first = true;
            for (auto& el : sl->elements) {
                const Type* t = checkExpr(el.get(), context);
                if (first) { elem = t; first = false; }
            }
            return types_.set(elem);
        }
        case NodeKind::ArrayLitExpr: {
            auto* a = static_cast<ArrayLitExpr*>(e);
            const Type* elem = types_.unknownType();
            bool first = true;
            for (auto& el : a->elements) {
                const Type* t = checkExpr(el.get(), context);
                if (first) { elem = t; first = false; }
            }
            return types_.array(elem);
        }
        case NodeKind::DictLitExpr: {
            auto* d = static_cast<DictLitExpr*>(e);
            const Type* k = types_.unknownType();
            const Type* v = types_.unknownType();
            for (size_t i = 0; i < d->keys.size() && i < d->values.size(); ++i) {
                const Type* kt = checkExpr(d->keys[i].get(), context);
                const Type* vt = checkExpr(d->values[i].get(), context);
                if (i == 0) { k = kt; v = vt; }
            }
            return types_.dict(k, v);
        }
        case NodeKind::RangeExpr: {
            auto* r = static_cast<RangeExpr*>(e);
            checkExpr(r->lower.get(), context);
            checkExpr(r->upper.get(), context);
            return types_.unknownType(); // Range is a stdlib generic
        }
        case NodeKind::AsExpr: {
            auto* a = static_cast<AsExpr*>(e);
            checkExpr(a->expr.get(), context);
            const Type* target = resolveTypeRepr(a->type.get(), context);
            if (a->asKind == AsExpr::AsQuestion) return types_.optional(target);
            return target ? target : types_.unknownType();
        }
        case NodeKind::IsExpr: {
            auto* is = static_cast<IsExpr*>(e);
            checkExpr(is->expr.get(), context);
            resolveTypeRepr(is->type.get(), context);
            return types_.boolType();
        }
        case NodeKind::MemberExpr: {
            auto* m = static_cast<MemberExpr*>(e);
            const Type* base = checkExpr(m->base.get(), context);
            // 类型名作命名空间：`Type.case` / `Type.self` 等成员访问（规范 1.7
            // 枚举成员值）。裸类型名本身在引用处被解析为 Unknown，但作为成员
            // 访问的基时，应还原为其具名类型，使枚举 case 值可达。
            if (base && base->kind == TypeKind::Unknown &&
                m->base->kind == NodeKind::IdentExpr) {
                const std::string& bn = static_cast<IdentExpr*>(m->base.get())->name;
                if (const TypeRecord* brec = findType(bn))
                    base = types_.named(brec, bn);
            }
            // Tuple element access `t.0`: the member name is the index.
            if (base && base->kind == TypeKind::Tuple) {
                size_t idx = 0;
                if (parseTupleIndex(m->member, idx)) {
                    if (idx < base->elements.size()) {
                        m->semaType = base->elements[idx];
                        return m->semaType;
                    }
                    hadError_ = true;
                    diags_.reportError("tuple index out of range: '" + m->member +
                                           "' (tuple has " +
                                           std::to_string(base->elements.size()) +
                                           " element(s))",
                                       rangeOf(e));
                    return types_.unknownType();
                }
                // Labelled element: `pair.code` names the element by its label
                // (规范 2.9). An unknown label is an error rather than a silent
                // fallback, since a tuple has no members beyond its elements.
                for (size_t i = 0; i < base->labels.size() && i < base->elements.size(); ++i) {
                    if (base->labels[i] == m->member) {
                        m->semaType = base->elements[i];
                        return m->semaType;
                    }
                }
                hadError_ = true;
                diags_.reportError("tuple has no element '" + m->member + "'",
                                   rangeOf(e));
                return types_.unknownType();
            }
            const Type* mt = resolveMemberType(base, m->member, e, context);
            if (m->optionalChain) return types_.optional(mt);
            return mt ? mt : types_.unknownType();
        }
        case NodeKind::SubscriptExpr: {
            auto* s = static_cast<SubscriptExpr*>(e);
            const Type* base = checkExpr(s->base.get(), context);
            for (auto& idx : s->indices) checkExpr(idx.get(), context);
            if (!base) return types_.unknownType();
            if (base->kind == TypeKind::Optional) base = base->element;
            if (base->kind == TypeKind::Array) return base->element;
            if (base->kind == TypeKind::Dict) return base->value;
            return types_.unknownType();
        }
        case NodeKind::GenericExpr: {
            auto* g = static_cast<GenericExpr*>(e);
            for (auto& a : g->args) resolveTypeRepr(a.get(), context);
            return checkExpr(g->base.get(), context);
        }
        case NodeKind::AssignmentExpr: {
            auto* a = static_cast<AssignmentExpr*>(e);
            const Type* lt = checkExpr(a->lhs.get(), context);
            const Type* rt = checkExpr(a->rhs.get(), context);
            if (a->lhs && a->lhs->kind == NodeKind::IdentExpr) {
                const std::string& nm = static_cast<IdentExpr*>(a->lhs.get())->name;
                bool isLet = false;
                if (const Symbol* s = locals_.lookup(nm)) isLet = s->isLet;
                else if (const Symbol* g = globals_.lookup(nm)) isLet = g->isLet;
                if (isLet) {
                    // `let x: Int` 声明后尚未初始化：这次赋值就是初始化，
                    // 合法且只允许一次（规范 1.3）。
                    if (!a->isCompound && pendingInit_.erase(nm) == 0) {
                        hadError_ = true;
                        diags_.reportError("cannot assign to immutable constant '" + nm + "'",
                                           rangeOf(a->lhs.get()));
                    }
                }
            }
            if (!a->isCompound)
                requireAssignable(lt, rt, e, "assignment");
            return lt ? lt : types_.unknownType();
        }
        case NodeKind::CallExpr: {
            auto* c = static_cast<CallExpr*>(e);
            // A call whose callee names a type is a constructor invocation
            // (`Point(x: 1)`, `MemoryPool<Int>(capacity: 8)`); it yields an
            // instance of that type rather than a function's result.
            if (c->callee && c->callee->kind == NodeKind::IdentExpr) {
                const std::string& n = static_cast<IdentExpr*>(c->callee.get())->name;
                if (const TypeRecord* rec = findType(n)) {
                    for (auto& a : c->arguments) checkExpr(a.get(), context);
                    return types_.named(rec, n);
                }
                // A built-in scalar type used as a callee is a conversion:
                // `Int(x)`, `Double(n)`, `Char("A")`, `String(1)`. It yields
                // the named type instead of calling a function.
                TypeKind bk;
                if (c->arguments.size() == 1 && builtinTypeFromName(n, bk)) {
                    const Type* target = types_.primitive(bk);
                    const Type* src = checkExpr(c->arguments[0].get(), context);
                    if (bk == TypeKind::Char &&
                        c->arguments[0]->kind == NodeKind::StrLitExpr) {
                        // `Char("A")` takes the literal's first scalar.
                        charContext_ = true;
                        checkExpr(c->arguments[0].get(), context);
                        charContext_ = false;
                        return target;
                    }
                    if (bk == TypeKind::String && src &&
                        src->kind == TypeKind::Char)
                        return target;
                    return target;
                }
            }
            // `Enum.case(args...)` constructs an enum case with a payload. The
            // callee resolves to a member of the enum type, so recover the enum
            // here and require the payload arity to match the declaration.
            if (c->callee && c->callee->kind == NodeKind::MemberExpr) {
                auto* m = static_cast<MemberExpr*>(c->callee.get());
                if (m->base && m->base->kind == NodeKind::IdentExpr) {
                    const std::string& bn = static_cast<IdentExpr*>(m->base.get())->name;
                    if (const TypeRecord* rec = findType(bn)) {
                        if (rec->kind == TypeDeclKind::Enum) {
                            for (auto& a : c->arguments) checkExpr(a.get(), context);
                            for (size_t i = 0; i < rec->cases.size(); ++i) {
                                if (rec->cases[i].name != m->member) continue;
                                size_t want = rec->cases[i].associated.size();
                                if (c->arguments.size() != want) {
                                    hadError_ = true;
                                    diags_.reportError(
                                        "enum case '" + rec->name + "." + m->member +
                                        "' expects " + std::to_string(want) +
                                        " value(s), got " +
                                        std::to_string(c->arguments.size()));
                                }
                                return types_.named(rec, rec->name);
                            }
                        }
                    }
                }
            }
            const Type* callee = checkExpr(c->callee.get(), context);
            // `print` / `println` take any printable value and any number of
            // them (规范 12.1), so they bypass the ordinary argument check.
            if (c->callee && c->callee->kind == NodeKind::IdentExpr) {
                const std::string& pn = static_cast<IdentExpr*>(c->callee.get())->name;
                if (pn == "print" || pn == "println") {
                    for (auto& a : c->arguments) checkExpr(a.get(), context);
                    return types_.voidType();
                }
            }
            // A closure argument learns its parameter types from the callee's
            // signature, which is what makes `{ x in x * 3 }` type-check when
            // the parameter is declared as `(Int) -> Int` (规范 3.3/3.4).
            if (callee && (callee->kind == TypeKind::Function ||
                           callee->kind == TypeKind::Closure)) {
                for (size_t i = 0; i < c->arguments.size() && i < callee->elements.size(); ++i) {
                    Node* a = c->arguments[i].get();
                    if (!a || a->kind != NodeKind::ClosureExpr) continue;
                    const Type* want = callee->elements[i];
                    if (!want || (want->kind != TypeKind::Function &&
                                  want->kind != TypeKind::Closure))
                        continue;
                    auto* cl = static_cast<ClosureExpr*>(a);
                    if (cl->params.size() != want->elements.size()) continue;
                    for (size_t k = 0; k < cl->params.size(); ++k)
                        if (!cl->params[k].semaType)
                            cl->params[k].semaType = want->elements[k];
                }
            }
            // Type-check arguments.
            std::vector<const Type*> argTypes;
            argTypes.reserve(c->arguments.size());
            for (auto& a : c->arguments) argTypes.push_back(checkExpr(a.get(), context));
            // A call to a generic function instantiates it: the argument types
            // are the type arguments, which is what monomorphisation needs.
            if (c->callee && c->callee->kind == NodeKind::IdentExpr) {
                const std::string& fname =
                    static_cast<IdentExpr*>(c->callee.get())->name;
                if (const Symbol* fs = globals_.lookup(fname)) {
                    if (fs->kind == Symbol::Kind::Function && fs->function &&
                        !fs->function->genericParams.empty() &&
                        fs->function->genericParams.size() == argTypes.size()) {
                        std::vector<std::string> targs;
                        for (const Type* at : argTypes)
                            targs.push_back(typeToString(inferTypeArgument(at)));
                        recordGenericInstance(fname, targs);
                        // Resolve the call's result type against this
                        // instantiation, so `let n = f(x)` learns the concrete
                        // type instead of inheriting the opaque one.
                        std::unordered_map<std::string, const Type*> saved =
                            genericBindings_;
                        for (size_t i = 0; i < targs.size(); ++i)
                            genericBindings_[fs->function->genericParams[i]] =
                                inferTypeArgument(argTypes[i]);
                        const Type* ret = fs->function->returnType
                            ? resolveTypeRepr(fs->function->returnType.get(), nullptr)
                            : types_.voidType();
                        genericBindings_ = saved;
                        callee = types_.function(argTypes, ret);
                    }
                }
            }
            if (callee && (callee->kind == TypeKind::Function ||
                           callee->kind == TypeKind::Closure)) {
                // 获取实际声明以识别变长参数（规范 3.1）：变长参数将吸收其位置
                // 及之后所有实参，从而允许“过多”的实参；带默认值的形参可被省略。
                const FunctionDecl* fdecl = nullptr;
                if (c->callee && c->callee->kind == NodeKind::IdentExpr) {
                    const std::string& fname =
                        static_cast<IdentExpr*>(c->callee.get())->name;
                    if (const Symbol* fs = globals_.lookup(fname)) {
                        if (fs->kind == Symbol::Kind::Function && fs->function)
                            fdecl = fs->function;
                    }
                }
                size_t nParams = callee->elements.size();
                size_t variadicIdx = (size_t)-1;
                if (fdecl) {
                    for (size_t i = 0; i < fdecl->params.size(); ++i) {
                        if (fdecl->params[i].isVariadic) { variadicIdx = i; break; }
                    }
                }
                // 变长参数：其位置及之后所有实参归入变长数组，故实参数量无上限。
                size_t maxArgs = nParams;
                if (variadicIdx != (size_t)-1) maxArgs = (size_t)-1;
                if (c->arguments.size() > maxArgs) {
                    hadError_ = true;
                    diags_.reportError("too many arguments in call (expected at most " +
                                       std::to_string(maxArgs) + ", got " +
                                       std::to_string(c->arguments.size()) + ")", rangeOf(e));
                }
                for (size_t i = 0; i < c->arguments.size(); ++i) {
                    const Type* paramType = nullptr;
                    if (variadicIdx != (size_t)-1 && i >= variadicIdx) {
                        // 变长参数：逐个实参按元素类型校验，而非整个数组类型。
                        const Type* arrT = callee->elements[variadicIdx];
                        paramType = (arrT && arrT->element) ? arrT->element
                                                           : types_.unknownType();
                    } else if (i < nParams) {
                        paramType = callee->elements[i];
                    } else {
                        paramType = types_.unknownType();
                    }
                    const Type* at2 = checkExpr(c->arguments[i].get(), context);
                    if (paramType) requireAssignable(paramType, at2, c->arguments[i].get(), "argument");
                }
                return callee->ret ? callee->ret : types_.unknownType();
            }
            if (callee && callee->kind == TypeKind::Optional && callee->element &&
                (callee->element->kind == TypeKind::Function ||
                 callee->element->kind == TypeKind::Closure)) {
                return callee->element->ret ? callee->element->ret : types_.unknownType();
            }
            return types_.unknownType();
        }
        case NodeKind::ClosureExpr: {
            auto* cl = static_cast<ClosureExpr*>(e);
            locals_.pushScope();
            std::vector<const Type*> params;
            for (auto& prm : cl->params) {
                // A parameter the call site has already typed (inferred from
                // the callee's signature) keeps that type; only a completely
                // unannotated parameter stays unknown.
                const Type* pt = prm.type ? resolveTypeRepr(prm.type.get(), context)
                                          : (prm.semaType ? prm.semaType
                                                          : types_.unknownType());
                prm.semaType = pt;
                params.push_back(pt);
                Symbol s; s.kind = Symbol::Kind::Parameter; s.type = pt; s.decl = e;
                locals_.declare(prm.internalName.empty() ? prm.externalName : prm.internalName, s);
            }
            const Type* ret = cl->returnType ? resolveTypeRepr(cl->returnType.get(), context)
                                             : nullptr;
            const Type* savedRet = currentReturn_;
            // A closure without a declared return type does not inherit the
            // enclosing function's return type.
            currentReturn_ = ret;
            checkStatements(cl->body, context, ret, currentThrows_);
            if (!ret) {
                // Infer from a trailing expression or return statement.
                for (auto it = cl->body.rbegin(); it != cl->body.rend(); ++it) {
                    if ((*it)->kind == NodeKind::ReturnStmt) {
                        ret = checkExpr(static_cast<ReturnStmt*>((*it).get())->value.get(), context);
                        break;
                    }
                }
                // A single-expression closure returns that expression
                // implicitly (规范 3.3), so `{ x in x * 3 }` has the type of
                // `x * 3` even though no `return` is written.
                if (!ret && !cl->body.empty() &&
                    cl->body.back()->kind == NodeKind::ExprStmt) {
                    auto* es = static_cast<ExprStmt*>(cl->body.back().get());
                    ret = checkExpr(es->expr.get(), context);
                }
            }
            currentReturn_ = savedRet;
            locals_.popScope();
            return types_.closure(std::move(params), ret ? ret : types_.unknownType());
        }
        case NodeKind::IfExpr: {
            auto* ie = static_cast<IfExpr*>(e);
            if (ie->condition && ie->condition->kind != NodeKind::VarDecl)
                checkExpr(ie->condition.get(), context);
            checkStatements(ie->thenBody, context, currentReturn_, currentThrows_);
            const Type* thenT = types_.unknownType();
            if (!ie->thenBody.empty()) {
                Node* last = ie->thenBody.back().get();
                if (last->kind == NodeKind::ExprStmt)
                    thenT = checkExpr(static_cast<ExprStmt*>(last)->expr.get(), context);
            }
            if (ie->elseBranch) checkStatement(ie->elseBranch.get(), context, currentReturn_, currentThrows_);
            return thenT;
        }
        default:
            return types_.unknownType();
    }
}

} // namespace suki
