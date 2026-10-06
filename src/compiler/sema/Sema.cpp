#include "compiler/sema/Sema.h"

#include <algorithm>
#include <functional>
#include <map>

#include "compiler/lexer/Lexer.h"
#include "compiler/parser/Parser.h"

#include <cerrno>
#include <cstdlib>

namespace suki {

static SourceRange rangeOf(Node* n) { return n ? n->range : SourceRange{}; }

// 访问控制级别推导（规范 10.1）：修饰符按出现顺序生效，后者覆盖前者；
// 未标注默认 Internal。`explicitLevel` 置为是否出现了访问级别修饰符，
// 用于区分「显式写 internal」与「未标注」。
static AccessLevel accessLevelFromModifiers(const std::vector<std::string>& mods,
                                            bool* explicitLevel = nullptr) {
    if (explicitLevel) *explicitLevel = false;
    AccessLevel lvl = AccessLevel::Internal;
    for (const auto& m : mods) {
        if (m == "public" || m == "open") {
            lvl = AccessLevel::Public;
            if (explicitLevel) *explicitLevel = true;
        } else if (m == "private") {
            lvl = AccessLevel::Private;
            if (explicitLevel) *explicitLevel = true;
        } else if (m == "fileprivate") {
            lvl = AccessLevel::Fileprivate;
            if (explicitLevel) *explicitLevel = true;
        } else if (m == "internal") {
            lvl = AccessLevel::Internal;
            if (explicitLevel) *explicitLevel = true;
        }
    }
    return lvl;
}

// 声明的访问级别（规范 §10.1）。被 `import` 载入的模块中，未显式标注访问级别
// 的声明即该模块导出的公开接口 —— 否则用户 import 之后连库的基础类型都无法
// 访问。显式标注（internal/fileprivate/private）的声明则严格按其标注执行。
static AccessLevel declaredAccessLevel(Node* n, const std::vector<std::string>& mods) {
    bool explicitLevel = false;
    AccessLevel lvl = accessLevelFromModifiers(mods, &explicitLevel);
    if (!explicitLevel && n && !n->sourceModule.empty()) lvl = AccessLevel::Public;
    return lvl;
}

// 声明所属模块（空 = 用户主模块）。
static std::string moduleOfDecl(Node* n) {
    return n ? n->sourceModule : std::string();
}

// 访问控制判定（规范 §10.1）：跨模块访问仅 public/open 可见；同模块内
// internal/fileprivate 可见（当前为单文件编译，fileprivate 与 internal 等价），
// private 由 checkMemberAccess 另行按所属类型判定。
bool Sema::isAccessible(const std::string& targetModule, AccessLevel access) const {
    if (access == AccessLevel::Public) return true;
    if (targetModule != currentModule_) return false; // 跨模块：非 public 不可见
    return true;
}

// 从 Decl 节点取出其修饰符向量（用于推导成员访问级别）。
static std::vector<std::string> nodeModifiers(Node* m) {
    if (!m) return {};
    switch (m->kind) {
        case NodeKind::VarDecl: return static_cast<VarDecl*>(m)->modifiers;
        case NodeKind::FunctionDecl: return static_cast<FunctionDecl*>(m)->modifiers;
        case NodeKind::InitDecl: return static_cast<InitDecl*>(m)->modifiers;
        case NodeKind::SubscriptDecl: return static_cast<SubscriptDecl*>(m)->modifiers;
        case NodeKind::EnumCaseDecl: return static_cast<EnumCaseDecl*>(m)->modifiers;
        case NodeKind::TypealiasDecl: return static_cast<TypealiasDecl*>(m)->modifiers;
        case NodeKind::AssociatedTypeDecl: return static_cast<AssociatedTypeDecl*>(m)->modifiers;
        // 类型声明（struct/class/actor/enum/protocol）同样带访问级别修饰符，
        // 规范 §10.1 的 `public struct` 需要据此推导。
        case NodeKind::StructDecl:
        case NodeKind::ClassDecl:
        case NodeKind::ActorDecl:
        case NodeKind::EnumDecl:
        case NodeKind::ProtocolDecl:
            return static_cast<TypeDecl*>(m)->modifiers;
        default: return {};
    }
}


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
        // 泛型约束（规范 2.1）：校验本次实例化的类型实参满足声明处约束。
        checkGenericConstraints(fn->genericConstraints, genericBindings_, fn);
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
    // 泛型约束（规范 2.1）：校验实例化的类型实参满足声明处约束。
    checkGenericConstraints(fn->genericConstraints, genericBindings_, fn);
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

void Sema::bindTypeParams(const std::vector<std::string>& params,
                          const std::vector<const Type*>& args) {
    bindingStack_.push_back(genericBindings_);
    for (size_t i = 0; i < params.size() && i < args.size(); ++i)
        genericBindings_[params[i]] = args[i];
}

void Sema::unbindTypeParams() {
    if (bindingStack_.empty()) return;
    genericBindings_ = bindingStack_.back();
    bindingStack_.pop_back();
}

// Re-resolve a function's signature under the current bindings and re-check its
// body. Members of a generic type share one AST across instantiations, so their
// annotated types must be refreshed immediately before each instance is lowered.
void Sema::resolveFunctionSignature(FunctionDecl* fn) {
    if (!fn) return;
    for (Param& prm : fn->params)
        prm.semaType = prm.type ? resolveTypeRepr(prm.type.get(), nullptr)
                                : prm.semaType;
    if (fn->returnType) {
        const Type* rt = resolveTypeRepr(fn->returnType.get(), nullptr);
        if (rt) fn->returnType->semaType = rt;
    }
    checkFunctionBody(fn, nullptr);
}

// Build (or reuse) the monomorphised record for `Box<Int>`: a copy of the generic
// record whose every member type is re-resolved with the type parameters bound to
// the concrete arguments, so fields store real values and methods return them.
const Type* Sema::monomorphiseGenericType(const TypeRecord* rec,
                                          const std::string& name,
                                          const std::vector<const Type*>& args) {
    std::string key = name + "<";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) key += ",";
        key += typeToString(args[i]);
    }
    key += ">";
    for (const auto& gi : genericTypeInstances_)
        if (gi.key == key) return gi.instanceType;

    // Bind the parameters for the duration of the re-resolution below.
    bindingStack_.push_back(genericBindings_);
    for (size_t i = 0; i < args.size() && i < rec->genericParams.size(); ++i)
        genericBindings_[rec->genericParams[i]] = args[i];

    // Built field by field rather than copied: GenericConstraint owns a NodePtr
    // and is therefore not copyable.
    auto* inst = new TypeRecord();
    inst->name = key;
    inst->kind = rec->kind;
    inst->decl = rec->decl;
    inst->superclass = rec->superclass;
    inst->protocols = rec->protocols;
    inst->isCEnum = rec->isCEnum;
    inst->origRec = rec;   // 回指原始声明，便于枚举穷举比较规范化
    // The instance is concrete: it has no parameters or constraints left to bind.
    for (const auto& m : rec->members) {
        TypeRecord::Member nm = m;
        nm.owner = inst;
        if (m.isFunction && m.decl && m.decl->kind == NodeKind::FunctionDecl) {
            auto* fd = static_cast<FunctionDecl*>(m.decl);
            std::vector<const Type*> pts;
            for (auto& p : fd->params)
                pts.push_back(p.type ? resolveTypeRepr(p.type.get(), nullptr)
                                     : (p.semaType ? p.semaType : types_.unknownType()));
            const Type* rt = fd->returnType
                ? resolveTypeRepr(fd->returnType.get(), nullptr)
                : types_.voidType();
            // Calling an async member yields a Future of its declared result, so
            // the instance's member type must say so for `await` to type-check.
            nm.type = types_.function(std::move(pts),
                                      fd->isAsync ? types_.future(rt) : rt);
        } else if (m.decl && m.decl->kind == NodeKind::VarDecl) {
            auto* vd = static_cast<VarDecl*>(m.decl);
            if (vd->type) {
                const Type* vt = resolveTypeRepr(vd->type.get(), nullptr);
                if (vt) nm.type = vt;
            }
        }
        inst->members.push_back(nm);
    }
    // 枚举：把 case 关联值类型按具体实参单态化后复制进实例记录。声明期关联
    // 类型（如 `T`）被解析为 Unknown（因彼时 T 尚未绑定），须在 genericBindings_
    // 已绑定 T→Int 的此刻从原始 EnumCaseDecl 节点重新解析，否则下沉会误判为裸
    // 枚举（lower 返回 i64 而非 {i64,i8*}）且载荷类型错误，泛型枚举完全不可用。
    if (rec->kind == TypeDeclKind::Enum && rec->decl &&
        rec->decl->kind == NodeKind::EnumDecl) {
        auto* ed = static_cast<EnumDecl*>(rec->decl);
        size_t ci = 0;
        for (auto& mnode : ed->members) {
            if (!mnode || mnode->kind != NodeKind::EnumCaseDecl) continue;
            auto* ec = static_cast<EnumCaseDecl*>(mnode.get());
            TypeRecord::EnumCaseInfo nc;
            nc.name = ec->name;
            nc.rawValue = (ci < rec->cases.size()) ? rec->cases[ci].rawValue : -1;
            for (auto& at : ec->associatedTypes)
                nc.associated.push_back(resolveTypeRepr(at.get(), nullptr));
            inst->cases.push_back(nc);
            ++ci;
        }
    }
    genericBindings_ = bindingStack_.back();
    bindingStack_.pop_back();

    const Type* instTy = types_.named(inst, key, args);
    monoRecords_.emplace_back(inst);

    GenericTypeInstance gi;
    gi.typeName = name;
    gi.key = key;
    gi.instanceType = instTy;
    gi.params = rec->genericParams;
    gi.args = args;
    gi.isClass = (rec->kind == TypeDeclKind::Class ||
                  rec->kind == TypeDeclKind::Actor);
    for (const Type* a : args) gi.typeArgs.push_back(typeToString(a));
    genericTypeInstances_.push_back(gi);
    return instTy;
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

bool Sema::unifyGenericParamNode(Node* declared, const Type* actual,
                                 const std::vector<std::string>& typeParams,
                                 std::unordered_map<std::string, const Type*>& bind) {
    if (!declared || !actual) return false;
    switch (declared->kind) {
        case NodeKind::NamedType: {
            auto* nt = static_cast<NamedType*>(declared);
            // A bare type variable (e.g. `T`, `U`): bind it to the concrete actual.
            if (nt->genericArgs.empty() &&
                std::find(typeParams.begin(), typeParams.end(), nt->name) !=
                    typeParams.end()) {
                if (bind.count(nt->name)) return false;
                if (actual->kind == TypeKind::Unknown) return false;
                bind[nt->name] = actual;
                return true;
            }
            // A generic container such as `Array<T>` / `Optional<T>` /
            // `Dictionary<K, V>`: recurse into its element(s).
            if (!nt->genericArgs.empty()) {
                bool ch = false;
                const Type* inner = nullptr;
                if (nt->name == "Array" || nt->name == "Set")
                    inner = (actual->kind == TypeKind::Array ||
                             actual->kind == TypeKind::Set) ? actual->element : nullptr;
                else if (nt->name == "Optional")
                    inner = actual->kind == TypeKind::Optional ? actual->element : nullptr;
                else if (nt->name == "Dictionary") {
                    inner = actual->kind == TypeKind::Dict ? actual->key : nullptr;
                    if (nt->genericArgs.size() >= 2 && actual->kind == TypeKind::Dict &&
                        actual->value)
                        ch |= unifyGenericParamNode(nt->genericArgs[1].get(),
                                                    actual->value, typeParams, bind);
                }
                if (inner && nt->genericArgs.size() >= 1)
                    ch |= unifyGenericParamNode(nt->genericArgs[0].get(), inner,
                                                typeParams, bind);
                return ch;
            }
            return false;
        }
        case NodeKind::FuncType: {
            auto* ft = static_cast<FuncType*>(declared);
            if (actual->kind != TypeKind::Function &&
                actual->kind != TypeKind::Closure) return false;
            if (ft->params.size() != actual->elements.size()) return false;
            bool ch = false;
            for (size_t i = 0; i < ft->params.size(); ++i)
                if (ft->params[i] && actual->elements[i])
                    ch |= unifyGenericParamNode(ft->params[i].get(),
                                                actual->elements[i], typeParams, bind);
            if (ft->ret && actual->ret)
                ch |= unifyGenericParamNode(ft->ret.get(), actual->ret,
                                            typeParams, bind);
            return ch;
        }
        case NodeKind::ArrayType: {
            auto* at = static_cast<ArrayType*>(declared);
            if (actual->kind != TypeKind::Array || !at->element) return false;
            return unifyGenericParamNode(at->element.get(), actual->element,
                                        typeParams, bind);
        }
        case NodeKind::OptionalType: {
            auto* ot = static_cast<OptionalType*>(declared);
            if (actual->kind != TypeKind::Optional || !ot->wrapped) return false;
            return unifyGenericParamNode(ot->wrapped.get(), actual->element,
                                        typeParams, bind);
        }
        default:
            return false;
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

void Sema::analyze(NodeList& decls) {
    hadError_ = false;
    // 宏展开（规范 5.6）：在类型解析与符号收集之前就地展开 freestanding 宏。
    expandMacros(decls);
    globals_.pushScope();
    locals_.pushScope();
    registerBuiltins();

    // Pass 1: create type records for every named type declaration.
    // Declarations belonging to the imported stdlib prelude (index < stdlibDeclCount_)
    // are trusted and may reference unsafe types (e.g. UnsafeMutablePointer).
    for (size_t i = 0; i < decls.size(); ++i) {
        auto& d = decls[i];
        if (!d) continue;
        unsafeContext_ = (i < stdlibDeclCount_);
        checkingStdlib_ = (i < stdlibDeclCount_);
        currentModule_ = moduleOfDecl(d.get());
        switch (d->kind) {
            case NodeKind::StructDecl: case NodeKind::EnumDecl:
            case NodeKind::ClassDecl: case NodeKind::ActorDecl:
            case NodeKind::ProtocolDecl:
                collectTypeDecl(d.get(), i < stdlibDeclCount_);
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
        // 标准库类型成员的继承解析同样处于受信任上下文。
        checkingStdlib_ = rec->isStdlib;
        currentModule_ = moduleOfDecl(rec->decl);
        unsafeContext_ = rec->isStdlib;
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
        // 标准库类型的成员（含属性类型标注，如 baseAddress: UnsafePointer<T>）
        // 处于受信任上下文，可引用 unsafe 类型。
        checkingStdlib_ = rec->isStdlib;
        currentModule_ = moduleOfDecl(rec->decl);
        unsafeContext_ = rec->isStdlib;
        if (rec->decl) collectMembers(*rec, static_cast<TypeDecl*>(rec->decl));
    }

    // 枚举原始值（@enum(C)，规范 1.5）：未显式赋值的 case 取上一个值 +1，
    // 从 0 起始顺序编号。
    for (auto& kv : typeIndex_) {
        TypeRecord* rec = kv.second;
        if (rec->kind != TypeDeclKind::Enum) continue;
        int64_t next = 0;
        for (auto& c : rec->cases) {
            if (c.rawValue < 0) c.rawValue = next;
            next = c.rawValue + 1;
        }
    }

    // Pass 3.5: fold extension members (and the conformances they declare)
    // into the extended type, then inherit protocol default implementations.
    // After this the member list of every type is flat, so code generation
    // and member lookup need no special handling for extensions (规范 2.6/4.5).
    mergeExtensions(decls);
    mergeDefaultImplementations();
    // 继承校验（规范 4.3）：override / final 语义，需在成员收集完成后进行。
    checkOverrides();
    // 构造器规则（规范 4.4）：required / convenience 语义。
    checkInitRules();

    // Pass 4: collect global functions and variables.
    // (stdlib decls may declare functions/initialisers returning unsafe types.)
    for (size_t i = 0; i < decls.size(); ++i) {
        auto& d = decls[i];
        if (!d) continue;
        unsafeContext_ = (i < stdlibDeclCount_);
        checkingStdlib_ = (i < stdlibDeclCount_);
        currentModule_ = moduleOfDecl(d.get());
        if (d->kind == NodeKind::FunctionDecl) {
            collectFunction(static_cast<FunctionDecl*>(d.get()), nullptr);
        } else if (d->kind == NodeKind::VarDecl) {
            collectGlobalVar(static_cast<VarDecl*>(d.get()));
        }
    }

    // Pass 5: check function bodies.
    // 不透明返回类型（规范 5.5）推断必须在函数体检查之前完成，以便前向引用的
    // 调用点也能解析到底层具体类型。
    inferOpaqueReturnTypes(decls);
    unsafeContext_ = false;
    for (auto& kv : typeIndex_) {
        TypeRecord* rec = kv.second;
        if (!rec->decl) continue;
        // 标准库类型的方法体可引用 unsafe 类型（如 UnsafeMutablePointer）。
        unsafeContext_ = rec->isStdlib;
        checkingStdlib_ = rec->isStdlib;
        currentModule_ = moduleOfDecl(rec->decl);
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
    for (size_t i = 0; i < decls.size(); ++i) {
        auto& d = decls[i];
        unsafeContext_ = (i < stdlibDeclCount_);
        checkingStdlib_ = (i < stdlibDeclCount_);
        currentModule_ = moduleOfDecl(d.get());
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
    if (!genericInstances_.empty()) {
        // 单态化重新检查泛型函数体（含标准库泛型类型的方法）；放宽 unsafe 约束，
        // 因这些实例化多来自受信任的标准库。
        bool savedUnsafe = unsafeContext_;
        unsafeContext_ = true;
        monomorphise(decls);
        unsafeContext_ = savedUnsafe;
    }

    // 裸机属性（规范 §8.7）在类型与泛型实例化都就绪后校验，
    // 以便 @global_allocator 的一致性检查能看到已解析的协议列表。
    validateBareMetalAttrs(decls);

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
            // 关联类型绑定（规范 2.2）：带 associatedtype 的协议，遵循类型须为
            // 每个关联类型提供绑定（类型内 typealias）。泛型类型留待类型推断，
            // 此处仅对具体（非泛型）类型强制显式绑定。
            if (!proto->associatedTypes.empty() && rec->genericParams.empty()) {
                for (const auto& at : proto->associatedTypes) {
                    bool bound = false;
                    for (const TypeRecord* r = rec; r && !bound; r = r->superclass) {
                        for (auto& mm : r->members) {
                            if (mm.decl && mm.decl->kind == NodeKind::TypealiasDecl &&
                                mm.name == at) { bound = true; break; }
                        }
                    }
                    if (!bound) {
                        hadError_ = true;
                        diags_.reportError("type '" + rec->name +
                            "' does not conform to protocol '" + proto->name +
                            "': missing associated type binding for '" + at + "'",
                            rangeOf(rec->decl));
                    }
                }
            }
        }
    }
}

void Sema::checkMemberAccess(const TypeRecord* owner, const TypeRecord::Member* m, Node* at) {
    (void)owner;
    if (!m) return;
    // 规范 10.1：跨模块仅 `public`/`open` 可见 —— internal/fileprivate/private
    // 都不构成模块的公开接口。
    if (!isAccessible(m->module, m->access)) {
        hadError_ = true;
        diags_.reportError("'" + m->name + "' is inaccessible: it is not 'public' "
                           "and belongs to another module", rangeOf(at));
        return;
    }
    // 规范 10.1：`private` 成员仅当 currentType_ 等于其所属类型时可见；
    // `fileprivate`/`internal`/`public` 在同模块内可见。
    if (m->access == AccessLevel::Private && currentType_ != m->owner) {
        hadError_ = true;
        diags_.reportError("'" + m->name + "' is private and cannot be accessed from here",
                           rangeOf(at));
    }
}

bool Sema::typeConformsTo(const Type* t, const std::string& protoName) const {
    if (!t || t->kind != TypeKind::Named || !t->record) return false;
    for (const TypeRecord* r = t->record; r; r = r->superclass)
        for (const TypeRecord* p : r->protocols)
            if (p && p->name == protoName) return true;
    return false;
}

void Sema::checkGenericConstraints(const std::vector<GenericConstraint>& cs,
                                   const std::unordered_map<std::string, const Type*>& bindings,
                                   Node* at) {
    for (const auto& c : cs) {
        // 左端应为类型形参名（NamedType）。
        std::string lhsName = (c.lhs && c.lhs->kind == NodeKind::NamedType)
            ? static_cast<NamedType*>(c.lhs.get())->name : std::string();
        auto it = bindings.find(lhsName);
        if (it == bindings.end()) continue; // 形参未在本实参列表中绑定，跳过
        const Type* lt = it->second;
        if (c.sameType) {
            const Type* rt = c.rhs ? resolveTypeRepr(c.rhs.get(), nullptr) : nullptr;
            if (rt && typeToString(lt) != typeToString(rt)) {
                hadError_ = true;
                diags_.reportError("generic requirement '" + lhsName + " == " +
                    typeToString(rt) + "' not satisfied by '" + typeToString(lt) + "'",
                    rangeOf(at));
            }
        } else {
            std::string protoName = (c.rhs && c.rhs->kind == NodeKind::NamedType)
                ? static_cast<NamedType*>(c.rhs.get())->name : std::string();
            if (!protoName.empty() && !typeConformsTo(lt, protoName)) {
                hadError_ = true;
                diags_.reportError("type '" + typeToString(lt) +
                    "' does not conform to required protocol '" + protoName + "'",
                    rangeOf(at));
            }
        }
    }
}

void Sema::checkInitRules() {
    // 规范 4.4：required / convenience 构造器的语义校验。
    struct InitInfo { std::string sig; bool required; bool convenience; Node* decl; };
    std::unordered_map<const TypeRecord*, std::vector<InitInfo>> inits;
    for (auto& kv : typeIndex_) {
        TypeRecord* rec = kv.second;
        if (!rec->decl || rec->kind != TypeDeclKind::Class) continue;
        for (auto& m : rec->members) {
            if (m.name != "init" || !m.isFunction || !m.decl ||
                m.decl->kind != NodeKind::InitDecl) continue;
            auto* id = static_cast<InitDecl*>(m.decl);
            std::string sig;
            if (m.type) for (size_t i = 0; i < m.type->elements.size(); ++i) {
                if (i) sig += ":";
                sig += typeToString(m.type->elements[i]);
            }
            inits[rec].push_back({sig, id->isRequired, id->isConvenience, m.decl});
        }
    }
    // 某个类可见的 required 签名 = 自身 + 所有父类的 required 签名。
    auto requiredSigs = [&](const TypeRecord* rec) {
        std::unordered_set<std::string> s;
        for (const TypeRecord* r = rec; r; r = r->superclass) {
            auto it = inits.find(r);
            if (it == inits.end()) continue;
            for (auto& ii : it->second) if (ii.required) s.insert(ii.sig);
        }
        return s;
    };
    // 规则 A：每个子类必须重新声明父类链中所有 required 构造器。
    for (auto& kv : typeIndex_) {
        TypeRecord* rec = kv.second;
        if (!rec->decl || rec->kind != TypeDeclKind::Class || !rec->superclass) continue;
        std::unordered_set<std::string> req = requiredSigs(rec->superclass);
        if (req.empty()) continue;
        auto it = inits.find(rec);
        std::unordered_set<std::string> own;
        if (it != inits.end()) for (auto& ii : it->second) if (ii.required) own.insert(ii.sig);
        for (auto& sig : req)
            if (own.find(sig) == own.end()) {
                hadError_ = true;
                diags_.reportError("class '" + rec->name +
                    "' must provide a 'required' initializer matching '" + sig + "'",
                    rangeOf(rec->decl));
            }
    }
    // 规则 B：convenience 构造器必须通过 `self.init(...)` 委派给另一构造器。
    for (auto& kv : inits) {
        for (auto& ii : kv.second) {
            if (!ii.convenience) continue;
            auto* id = static_cast<InitDecl*>(ii.decl);
            if (id->body.empty()) continue;
            Node* first = id->body.front().get();
            if (first && first->kind == NodeKind::ExprStmt)
                first = static_cast<ExprStmt*>(first)->expr.get();
            bool delegates = false;
            if (first && first->kind == NodeKind::CallExpr) {
                auto* c = static_cast<CallExpr*>(first);
                if (c->callee && c->callee->kind == NodeKind::MemberExpr) {
                    auto* me = static_cast<MemberExpr*>(c->callee.get());
                    if (me->member == "init" && me->base && me->base->kind == NodeKind::IdentExpr &&
                        static_cast<IdentExpr*>(me->base.get())->name == "self")
                        delegates = true;
                } else if (c->callee && c->callee->kind == NodeKind::IdentExpr &&
                           static_cast<IdentExpr*>(c->callee.get())->name == "init") {
                    delegates = true;
                }
            }
            if (!delegates) {
                hadError_ = true;
                diags_.reportError("convenience initializer must delegate to another "
                                   "initializer via 'self.init(...)'", rangeOf(ii.decl));
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

    // Foreign builtins backed by the C runtime (src/runtime/runtime.c). These are
    // real function declarations so the code generator knows their flags (async /
    // foreign / cdecl symbol) and the checker can type incoming arguments.
    auto intMutRef = types_.ref(RefKind::Mut, types_.intType());
    auto addBuiltinFn = [&](const char* name, std::vector<const Type*> params,
                           const Type* ret, bool async = false,
                           const std::string& cname = "") {
        auto* fn = new FunctionDecl();
        fn->name = name;
        fn->isAsync = async;
        fn->isForeign = true;
        fn->cdeclName = cname;
        for (size_t i = 0; i < params.size(); ++i) {
            // Param holds a unique_ptr (default value), so it is not copyable;
            // build it in place rather than push_back a temporary.
            fn->params.emplace_back();
            Param& p = fn->params.back();
            p.internalName = "a" + std::to_string(i);
            p.semaType = params[i];
        }
        if (ret) {
            // The return type is a type-representation node carrying the resolved
            // type; code generation reads `returnType->semaType`.
            auto* tr = new NamedType();
            tr->name = typeToString(ret);
            tr->semaType = ret;
            fn->returnType.reset(tr);
        }
        Symbol s;
        s.kind = Symbol::Kind::Function;
        s.function = fn;
        s.type = types_.function(params, ret);
        globals_.declare(name, s);
    };
    // Synchronous runtime primitives used by the concurrency stress test.
    addBuiltinFn("atomicAdd",  { intMutRef, types_.intType() }, types_.intType(),
                 false, "suki_atomic_add_i64");
    addBuiltinFn("mutexLock",   { intMutRef }, types_.voidType(),
                 false, "suki_spin_lock");
    addBuiltinFn("mutexUnlock", { intMutRef }, types_.voidType(),
                 false, "suki_spin_unlock");
    // `await sleep(ms)` suspends; sleep is an async foreign builtin.
    addBuiltinFn("sleep", { types_.intType() }, types_.voidType(),
                 true, "suki_sleep");

    // ─── Channel<T> ─────────────────────────────────────────────────────────
    // A bounded, blocking FIFO backed by the C runtime. An instance stores only
    // the runtime handle; `send` / `receive` block the calling thread, which is
    // the right behaviour inside an async function (it runs on its own thread).
    // The bodies are emitted by the code generator against the runtime, so the
    // declarations carry no SukiCode body.
    {
        auto rec = std::make_unique<TypeRecord>();
        rec->name = "Channel";
        rec->kind = TypeDeclKind::Struct;
        rec->genericParams = { "T" };

        auto typeRepr = [](const char* n) {
            auto* nt = new NamedType();
            nt->name = n;
            return nt;
        };

        auto* handleDecl = new VarDecl();
        handleDecl->name = "handle";
        handleDecl->type.reset(typeRepr("OpaquePointer"));
        TypeRecord::Member handleMem;
        handleMem.isFunction = false;
        handleMem.name = "handle";
        handleMem.type = types_.primitive(TypeKind::OpaquePointer);
        handleMem.decl = handleDecl;
        rec->members.push_back(handleMem);

        // `send` / `receive` / `close` are async: they may block, so callers
        // `await` them and the result type is a Future (规范 7.5).
        // func send(_ value: T)
        {
            auto* fd = new FunctionDecl();
            fd->name = "send";
            fd->isAsync = true;
            fd->params.emplace_back();
            Param& p = fd->params.back();
            p.externalName = "_";
            p.internalName = "value";
            p.type.reset(typeRepr("T"));
            fd->returnType.reset(typeRepr("Void"));
            TypeRecord::Member m;
            m.isFunction = true; m.name = "send";
            m.type = types_.function({ types_.unknownType() },
                                     types_.future(types_.voidType()));
            m.decl = fd;
            rec->members.push_back(m);
        }
        // func receive() -> T
        {
            auto* fd = new FunctionDecl();
            fd->name = "receive";
            fd->isAsync = true;
            fd->returnType.reset(typeRepr("T"));
            TypeRecord::Member m;
            m.isFunction = true; m.name = "receive";
            m.type = types_.function({}, types_.future(types_.unknownType()));
            m.decl = fd;
            rec->members.push_back(m);
        }
        // func close()
        {
            auto* fd = new FunctionDecl();
            fd->name = "close";
            fd->isAsync = true;
            fd->returnType.reset(typeRepr("Void"));
            TypeRecord::Member m;
            m.isFunction = true; m.name = "close";
            m.type = types_.function({}, types_.future(types_.voidType()));
            m.decl = fd;
            rec->members.push_back(m);
        }

        TypeRecord* raw = rec.get();
        typeIndex_["Channel"] = raw;
        ownedRecords_.push_back(std::move(rec));
        Symbol s; s.kind = Symbol::Kind::Type; s.record = raw;
        globals_.declare("Channel", s);
    }

    // ─── Task / TaskGroup (structured concurrency) ──────────────────────────
    // Both are single-field wrappers around a runtime handle. `Task { ... }` runs
    // a closure on its own thread; a TaskGroup records each child's Future so
    // `waitForAll` can join them — the guarantee behind structured concurrency
    // that no child outlives the group that spawned it.
    auto voidRepr = []() { auto* n = new NamedType(); n->name = "Void"; return n; };

    auto addHandleStruct = [&](const char* name,
                               std::vector<TypeRecord::Member> extras) {
        auto rec = std::make_unique<TypeRecord>();
        rec->name = name;
        rec->kind = TypeDeclKind::Struct;
        auto* hd = new VarDecl();
        hd->name = "handle";
        auto* hr = new NamedType(); hr->name = "OpaquePointer";
        hd->type.reset(hr);
        TypeRecord::Member hm;
        hm.isFunction = false; hm.name = "handle";
        hm.type = types_.primitive(TypeKind::OpaquePointer);
        hm.decl = hd;
        rec->members.push_back(hm);
        for (auto& m : extras) rec->members.push_back(m);
        TypeRecord* raw = rec.get();
        typeIndex_[name] = raw;
        ownedRecords_.push_back(std::move(rec));
        Symbol s; s.kind = Symbol::Kind::Type; s.record = raw;
        globals_.declare(name, s);
        return raw;
    };

    // Task { ... } — fire-and-forget; `wait()` blocks until it completes.
    {
        std::vector<TypeRecord::Member> ms;
        auto* fd = new FunctionDecl();
        fd->name = "wait";
        fd->returnType.reset(voidRepr());
        TypeRecord::Member m;
        m.isFunction = true; m.name = "wait";
        m.type = types_.function({}, types_.voidType());
        m.decl = fd;
        ms.push_back(m);
        addHandleStruct("Task", ms);
    }

    // TaskGroup<T> — `addTask { ... }` spawns a child producing a T, and
    // `waitForAll` joins them. `T` lets `for await` collect the children's
    // results; a group used without type arguments behaves as before.
    {
        std::vector<TypeRecord::Member> ms;
        {
            auto* fd = new FunctionDecl();
            fd->name = "addTask";
            fd->params.emplace_back();
            Param& p = fd->params.back();
            p.externalName = "_";
            p.internalName = "body";
            auto* ft = new FuncType();
            auto* tRepr = new NamedType(); tRepr->name = "T";
            ft->ret.reset(tRepr);
            p.type.reset(ft);
            fd->returnType.reset(voidRepr());
            TypeRecord::Member m;
            m.isFunction = true; m.name = "addTask";
            m.type = types_.function({ types_.function({}, types_.unknownType()) },
                                     types_.voidType());
            m.decl = fd;
            ms.push_back(m);
        }
        {
            auto* fd = new FunctionDecl();
            fd->name = "waitForAll";
            fd->returnType.reset(voidRepr());
            TypeRecord::Member m;
            m.isFunction = true; m.name = "waitForAll";
            m.type = types_.function({}, types_.voidType());
            m.decl = fd;
            ms.push_back(m);
        }
        TypeRecord* tgRec = addHandleStruct("TaskGroup", ms);
        // `TaskGroup<T>` is instantiated per child-result type so `for await`
        // knows how large each result is.
        tgRec->genericParams = { "T" };
    }

    // `withTaskGroup { group in ... }` — runs the body with a fresh group and
    // waits for every child before returning. It is async so callers `await` it.
    {
        auto* fd = new FunctionDecl();
        fd->name = "withTaskGroup";
        fd->isAsync = true;
        fd->params.emplace_back();
        Param& p = fd->params.back();
        p.externalName = "_";
        p.internalName = "body";
        auto* ft = new FuncType();
        auto* tgRepr = new NamedType(); tgRepr->name = "TaskGroup";
        ft->params.emplace_back(tgRepr);
        ft->ret.reset(voidRepr());
        p.type.reset(ft);
        fd->returnType.reset(voidRepr());
        fd->params[0].semaType =
            types_.function({ types_.named(findType("TaskGroup"), "TaskGroup") },
                            types_.voidType());

        Symbol s;
        s.kind = Symbol::Kind::Function;
        s.function = fd;
        s.type = types_.function(
            { types_.function({ types_.named(findType("TaskGroup"), "TaskGroup") },
                              types_.voidType()) },
            types_.voidType());
        globals_.declare("withTaskGroup", s);
    }
}

void Sema::collectTypeDecl(Node* decl, bool isStdlib) {
    auto* td = static_cast<TypeDecl*>(decl);
    auto rec = std::make_unique<TypeRecord>();
    rec->name = td->name;
    rec->decl = td;
    rec->isStdlib = isStdlib;            // 来自受信任标准库（可定义 unsafe 类型）
    switch (td->kind) {
        case NodeKind::StructDecl: rec->kind = TypeDeclKind::Struct; break;
        case NodeKind::EnumDecl: rec->kind = TypeDeclKind::Enum; break;
        case NodeKind::ClassDecl: rec->kind = TypeDeclKind::Class; break;
        case NodeKind::ActorDecl: rec->kind = TypeDeclKind::Actor; break;
        case NodeKind::ProtocolDecl: rec->kind = TypeDeclKind::Protocol; break;
        default: rec->kind = TypeDeclKind::Struct; break;
    }
    rec->genericParams = td->genericParams;
    rec->genericConstraints = std::move(td->genericConstraints);
    rec->isCEnum = td->isCEnum;          // @enum(C)（规范 1.5）

    TypeRecord* raw = rec.get();
    typeIndex_[td->name] = raw;
    // Own the record for the session.
    ownedRecords_.push_back(std::move(rec));

    // Annotate the declaration so the code generator can recover the record
    // from the AST alone (IRGenerator never sees Sema's tables directly).
    td->semaType = types_.named(raw, td->name);

    Symbol s; s.kind = Symbol::Kind::Type; s.record = raw; s.decl = td;
    // 访问控制（规范 §10.1）。
    s.access = declaredAccessLevel(td, nodeModifiers(td));
    s.module = moduleOfDecl(td);
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
            // 成员自身没有模块标记，其可见性由**所属类型**的模块决定（规范 §10.1）：
            // 类型的模块决定成员是否构成公开接口。
            mem.access = declaredAccessLevel(rec.decl ? rec.decl : m, nodeModifiers(m));
            mem.module = moduleOfDecl(rec.decl ? rec.decl : m);
            mem.owner = &rec;
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
        // 规范 7.4（actor 隔离）：从外部调用 actor 的方法要跨越隔离边界，因此是
        // 异步的——成员类型的返回值为 Future<R>，调用方须 `await` 取结果。
        if (rec.kind == TypeDeclKind::Actor)
            mem.type = types_.function(params, types_.future(ret));
        else
            mem.type = types_.function(std::move(params), ret);
        fn->semaType = mem.type; // 缓存函数类型对象，供 some 推断改写 ret
        if (rec.isProtocol()) {
            // A default implementation (a body present) is recorded on the
            // protocol so conforming types can inherit it; a pure requirement
            // (no body) is only a requirement to be satisfied elsewhere.
            fn->isDefaultImpl = !fn->body.empty();
            // 成员自身没有模块标记，其可见性由**所属类型**的模块决定（规范 §10.1）：
            // 类型的模块决定成员是否构成公开接口。
            mem.access = declaredAccessLevel(rec.decl ? rec.decl : m, nodeModifiers(m));
            mem.module = moduleOfDecl(rec.decl ? rec.decl : m);
            mem.owner = &rec;
            rec.requirements.push_back(mem);
        } else {
            // 成员自身没有模块标记，其可见性由**所属类型**的模块决定（规范 §10.1）：
            // 类型的模块决定成员是否构成公开接口。
            mem.access = declaredAccessLevel(rec.decl ? rec.decl : m, nodeModifiers(m));
            mem.module = moduleOfDecl(rec.decl ? rec.decl : m);
            mem.owner = &rec;
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
    } else if (m->kind == NodeKind::TypealiasDecl) {
        // 类型内 typealias（规范 2.2）：作为成员登记，使其可在关联类型绑定
        // 检查与成员查找中被发现。
        auto* ta = static_cast<TypealiasDecl*>(m);
        TypeRecord::Member mem;
        mem.name = ta->name;
        mem.isFunction = false;
        mem.decl = ta;
        mem.type = ta->underlying ? resolveTypeRepr(ta->underlying.get(), &rec)
                                   : types_.unknownType();
        // 成员自身没有模块标记，其可见性由**所属类型**的模块决定（规范 §10.1）：
            // 类型的模块决定成员是否构成公开接口。
            mem.access = declaredAccessLevel(rec.decl ? rec.decl : m, nodeModifiers(m));
            mem.module = moduleOfDecl(rec.decl ? rec.decl : m);
        mem.owner = &rec;
        rec.members.push_back(std::move(mem));
    } else if (m->kind == NodeKind::EnumCaseDecl) {
        auto* ec = static_cast<EnumCaseDecl*>(m);
        TypeRecord::EnumCaseInfo ci;
        ci.name = ec->name;
        ci.rawValue = ec->rawValue;   // @enum(C) 显式原始值（规范 1.5）
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
    // 访问控制（规范 §10.1）：记录访问级别与所属模块。
    s.access = declaredAccessLevel(fn, nodeModifiers(fn));
    s.module = moduleOfDecl(fn);
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
    // @convention(c|stdcall) 声明属性（规范 §6.3）：foreign / 外部函数声明可
    // 显式指定调用约定，使 codegen 对该符号发射匹配的 LLVM calling convention。
    CallConv cc = CallConv::Default;
    if (fn->convention == "c") cc = CallConv::C;
    else if (fn->convention == "stdcall") cc = CallConv::StdCall;
    else if (!fn->convention.empty())
        diags_.reportError("unknown calling convention '@convention(" +
                               fn->convention + ")'", rangeOf(fn));
    s.type = tc.function(std::move(params), ret, cc);
    globals_.declare(fn->name, s);
    // 缓存函数类型对象到 AST 节点，供不透明返回类型（some）推断后原地改写 ret。
    fn->semaType = s.type;
}

void Sema::collectGlobalVar(VarDecl* vd) {
    Symbol s;
    s.kind = Symbol::Kind::Variable;
    s.isLet = vd->isLet;
    s.isVar = !vd->isLet;
    s.isWeak = vd->isWeak;
    s.decl = vd;
    // 访问控制（规范 §10.1）。
    s.access = declaredAccessLevel(vd, nodeModifiers(vd));
    s.module = moduleOfDecl(vd);
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
            // Generic stdlib collection/optional names (Array/Set/Dictionary/
            // Optional) are builtin types but are intentionally absent from
            // builtinTypeFromName (which maps to a single concrete TypeKind), so
            // extend the gate here to let the per-name handling below run.
            if (builtinTypeFromName(name, bk) ||
                name == "Array" || name == "Set" ||
                name == "Dictionary" || name == "Optional") {
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
                if (name == "Unmanaged") {
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
            // `unsafe` 强制（规范 8.6）：裸指针/非托管类型仅可在 unsafe 块内使用。
            // 受信任标准库（checkingStdlib_）可自由定义/使用这些类型，故仅在
            // 用户代码且不在 unsafe 块内时强制。
            if ((name == "UnsafePointer" || name == "UnsafeMutablePointer" ||
                 name == "Unmanaged") && !unsafeContext_ && !checkingStdlib_) {
                diags_.reportError("'" + name +
                    "' may only be used inside an 'unsafe' block", rangeOf(repr));
            }

            // Owned<T>：唯一所有权（规范 §6.4）。编译器原生识别，因为规范强制
            // 在实例化 `Owned<class>` / `Owned<actor>` 时报错——这一规则无法仅靠
            // 泛型约束表达。当标准库 `memory` 模块尚未声明真实 `struct Owned<T>`
            // （当前情形）时在此合成；一旦 memory 模块给出该结构体，应改为依赖
            // 其泛型约束并在 monomorphise 处校验，避免与本地合成冲突。
            if (name == "Owned") {
                const Type* e = nt->genericArgs.empty()
                    ? types_.unknownType()
                    : resolveTypeRepr(nt->genericArgs[0].get(), context);
                // 规范 §6.4：Owned<T> 只能用于值类型或 Unmanaged<T>；T 为 class/actor
                // （由 ARC 管理、允许多引用）时禁止，否则唯一所有权会与 ARC 冲突，
                // 移动后原变量被禁用但其他 strong 引用仍存在，造成内存不安全。
                if (e && e->kind == TypeKind::Named && e->record &&
                    (e->record->kind == TypeDeclKind::Class ||
                     e->record->kind == TypeDeclKind::Actor))
                    diags_.reportError(
                        "'Owned<T>' cannot wrap a class/actor type; 'T' must be a "
                        "value type or Unmanaged<T> (spec §6.4)", rangeOf(repr));
                return types_.ref(RefKind::Owned, e);
            }

            // 规范 §8.7：`Never` 是内建 bottom 类型（`!`），没有类型记录，
            // 故按名字直接映射到 neverType()，使 `-> Never` 真正解析为发散类型
            // （此前会静默退化为 Unknown）。
            if (name == "Never") return types_.neverType();

            const TypeRecord* rec = findType(name);
            if (rec) {
                std::vector<const Type*> args;
                for (auto& a : nt->genericArgs) args.push_back(resolveTypeRepr(a.get(), context));
                // 泛型约束（规范 2.1）：校验类型实参满足声明处的约束。
                if (!rec->genericConstraints.empty() && args.size() == rec->genericParams.size()) {
                    std::unordered_map<std::string, const Type*> binds;
                    for (size_t i = 0; i < args.size(); ++i) binds[rec->genericParams[i]] = args[i];
                    checkGenericConstraints(rec->genericConstraints, binds, repr);
                }
                // 泛型类型实例化（规范 5.2）：`Box<Int>` 各自单态化出独立记录，
                // 字段与方法按具体类型解析，而非共用未解析的 `T`。
                if (!rec->genericParams.empty() &&
                    args.size() == rec->genericParams.size()) {
                    bool concrete = true;
                    for (const Type* a : args)
                        if (!a || a->kind == TypeKind::Unknown) { concrete = false; break; }
                    if (concrete) return monomorphiseGenericType(rec, name, args);
                }
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
            // @convention(c|stdcall) 类型属性（规范 §6.3）：映射为调用约定。
            CallConv cc = CallConv::Default;
            if (f->convention == "c") cc = CallConv::C;
            else if (f->convention == "stdcall") cc = CallConv::StdCall;
            else if (!f->convention.empty())
                diags_.reportError("unknown calling convention '@convention(" +
                                       f->convention + ")'", rangeOf(repr));
            return types_.function(std::move(params),
                                   resolveTypeRepr(f->ret.get(), context), cc);
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
    // A closure whose result is discarded adapts to a Void-returning function
    // type with the same parameters: `Task { f() }` is the natural spelling even
    // when `f()` produces a value the task does not use.
    if (to && from && to->kind == TypeKind::Function && to->ret &&
        to->ret->kind == TypeKind::Void &&
        (from->kind == TypeKind::Function || from->kind == TypeKind::Closure) &&
        from->elements.size() == to->elements.size())
        return;
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
void Sema::inferOpaqueReturnTypes(const NodeList& decls) {
    for (auto& d : decls) {
        if (d && d->kind == NodeKind::FunctionDecl)
            inferOpaqueReturnType(static_cast<FunctionDecl*>(d.get()), nullptr);
    }
    for (auto& kv : typeIndex_) {
        TypeRecord* rec = kv.second;
        if (!rec->decl) continue;
        for (auto& m : rec->members) {
            if (m.decl && m.decl->kind == NodeKind::FunctionDecl)
                inferOpaqueReturnType(static_cast<FunctionDecl*>(m.decl), rec);
        }
    }
}

const Type* Sema::inferOpaqueReturnType(FunctionDecl* fn, const TypeRecord* owner) {
    // 仅处理 `func f() -> some P`（规范 5.5）。
    if (!fn->returnType || fn->returnType->kind != NodeKind::OptionalType ||
        !static_cast<OptionalType*>(fn->returnType.get())->isOpaque)
        return nullptr;
    if (!fn->semaType || fn->semaType->kind != TypeKind::Function) return nullptr;

    // 构造最小上下文（参数 + self），遍历函数体收集 return 表达式类型。
    locals_.pushScope();
    if (owner) {
        Symbol self; self.kind = Symbol::Kind::Variable;
        self.type = types_.named(owner, owner->name); self.decl = fn;
        locals_.declare("self", self);
    }
    for (auto& prm : fn->params) {
        Symbol s; s.kind = Symbol::Kind::Parameter;
        s.type = resolveTypeRepr(prm.type.get(), owner);
        if (prm.isVariadic) s.type = types_.array(s.type);
        s.decl = fn;
        locals_.declare(prm.internalName, s);
    }

    const Type* underlying = nullptr;
    std::function<void(Node*)> visit = [&](Node* st) {
        if (!st) return;
        switch (st->kind) {
            case NodeKind::ReturnStmt: {
                auto* r = static_cast<ReturnStmt*>(st);
                if (r->value) {
                    const Type* t = checkExpr(r->value.get(), owner);
                    if (t && t->kind != TypeKind::Void && t->kind != TypeKind::Unknown && !underlying)
                        underlying = t;
                }
                break;
            }
            case NodeKind::BlockStmt:
                for (auto& s : static_cast<BlockStmt*>(st)->statements) visit(s.get());
                break;
            case NodeKind::IfStmt: {
                auto* is = static_cast<IfStmt*>(st);
                for (auto& s : is->thenBody) visit(s.get());
                if (is->elseBranch) visit(is->elseBranch.get());
                break;
            }
            case NodeKind::LoopStmt:
                for (auto& s : static_cast<LoopStmt*>(st)->body) visit(s.get());
                break;
            case NodeKind::WhileStmt:
                for (auto& s : static_cast<WhileStmt*>(st)->body) visit(s.get());
                break;
            case NodeKind::RepeatWhileStmt:
                for (auto& s : static_cast<RepeatWhileStmt*>(st)->body) visit(s.get());
                break;
            case NodeKind::ForInStmt:
                for (auto& s : static_cast<ForInStmt*>(st)->body) visit(s.get());
                break;
            case NodeKind::VarDecl: {
                // 声明体内 var 以便 return 引用前序变量时能推断其类型。
                auto* vd = static_cast<VarDecl*>(st);
                Symbol s; s.kind = Symbol::Kind::Variable;
                s.type = vd->type ? resolveTypeRepr(vd->type.get(), owner)
                                  : types_.unknownType();
                s.decl = vd;
                locals_.declare(vd->name, s);
                break;
            }
            default: break;
        }
    };
    for (auto& st : fn->body) visit(st.get());
    locals_.popScope();

    if (!underlying) return nullptr;

    // 约束校验：要求底层类型满足 `some P` 的约束 P。仅当约束是具体类型时检查
    // 相等/子类关系；协议约束（`some View`）暂略（教学场景常见）。
    auto* ot = static_cast<OptionalType*>(fn->returnType.get());
    if (ot->wrapped) {
        const Type* constraint = resolveTypeRepr(ot->wrapped.get(), owner);
        if (constraint && constraint->kind == TypeKind::Named &&
            underlying->kind == TypeKind::Named) {
            const TypeRecord* cr = constraint->record;
            const TypeRecord* ur = underlying->record;
            bool ok = (ur == cr);
            if (!ok && cr && ur) {
                for (const TypeRecord* s = ur->superclass; s; s = s->superclass)
                    if (s == cr) { ok = true; break; }
            }
            if (!ok) {
                hadError_ = true;
                diags_.reportError("opaque return type '" + typeToString(underlying) +
                    "' does not satisfy constraint '" + typeToString(constraint) + "'",
                    rangeOf(fn->returnType.get()));
            }
        }
    }

    // 原地改写函数类型对象的 ret 字段为底层具体类型，调用点（含前向引用）
    // 与 codegen 都能直接拿到 C。同时覆盖 `fn->returnType->semaType`，因为
    // codegen 从该字段取函数返回类型生成签名。
    const_cast<Type*>(fn->semaType)->ret = underlying;
    fn->returnType->semaType = underlying;
    opaqueReturnType_[fn] = underlying;
    return underlying;
}

void Sema::checkFunctionBody(FunctionDecl* fn, const TypeRecord* owner) {
    if (fn->isForeign) return; // external declaration: no body to check
    // 确定性赋值（§1.3）：待初始化集合按函数体重置，避免跨函数污染。
    pendingInit_.clear();
    currentType_ = owner;
    currentThrows_ = fn->isThrows;
    const Type* ret = fn->returnType ? resolveTypeRepr(fn->returnType.get(), owner)
                                     : types_.voidType();
    // 不透明返回类型（规范 5.5）：使用已推断的底层具体类型，确保函数体与调用点一致。
    auto oi = opaqueReturnType_.find(fn);
    if (oi != opaqueReturnType_.end()) ret = oi->second;
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
    // `-> Never`（规范 §8.7）同样豁免：发散函数靠 `loop {}` / trap 终止控制流，
    // 不存在可 `return` 的 Never 值，故不强求 return 语句。
    if (ret && ret->kind != TypeKind::Void && ret->kind != TypeKind::Unknown &&
        ret->kind != TypeKind::Never &&
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

// ─── 确定性赋值分析（规范 §1.3）──────────────────────────────────────────────
// `pendingInit_` 记录当前路径上「尚未赋值」的 let 名字。汇合多条控制流路径时，
// 「已赋值」要求**所有**路径都赋过值，因此未赋值集合取**并集**：任一支路仍未
// 赋值 ⇒ 汇合后仍视为未赋值。这正是规范「所有控制流路径上恰好赋值一次」的语义。
static std::unordered_set<std::string> mergePendingInit(
    const std::unordered_set<std::string>& a,
    const std::unordered_set<std::string>& b) {
    std::unordered_set<std::string> r = a;
    for (const auto& k : b) r.insert(k);
    return r;
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
            bool emptyCollectionLit = false;
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
                // Empty `[]` / `[:]` literals carry no elements to infer a type
                // from. When the annotation is itself a collection (Set / Dict /
                // Array), treat the literal as that empty collection so its type
                // matches the annotation (`let s: Set<Int> = []`).
                Node* initNode = vd->initializer.get();
                bool emptyArray = initNode->kind == NodeKind::ArrayLitExpr &&
                                  static_cast<ArrayLitExpr*>(initNode)->elements.empty();
                bool emptyDict = initNode->kind == NodeKind::DictLitExpr &&
                                 static_cast<DictLitExpr*>(initNode)->keys.empty();
                if (dt && (emptyArray || emptyDict) &&
                    (dt->kind == TypeKind::Set || dt->kind == TypeKind::Dict ||
                     dt->kind == TypeKind::Array)) {
                    init = dt;
                    initNode->semaType = dt;
                    emptyCollectionLit = true;
                }
            }
            if (declared && init && !emptyCollectionLit)
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
            // 确定性赋值（§1.3）：then/else 各自在当前状态的副本上分析，
            // 汇合时未赋值集合取并集 —— 只有两条路都赋值才算已赋值。
            {
                auto beforePending = pendingInit_;
                checkStatements(ifs->thenBody, context, fnReturnType, isThrowing);
                auto thenPending = pendingInit_;
                pendingInit_ = beforePending;
                if (ifs->elseBranch) {
                    checkStatement(ifs->elseBranch.get(), context, fnReturnType, isThrowing);
                    pendingInit_ = mergePendingInit(thenPending, pendingInit_);
                } else {
                    // 无 else：隐含的空分支保持进入前状态。
                    pendingInit_ = mergePendingInit(thenPending, beforePending);
                }
            }
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
            // 确定性赋值（§1.3）：与 IfStmt 相同的并集汇合规则。
            {
                auto beforePending = pendingInit_;
                checkStatements(e->thenBody, context, fnReturnType, isThrowing);
                auto thenPending = pendingInit_;
                pendingInit_ = beforePending;
                if (e->elseBranch) {
                    checkStatement(e->elseBranch.get(), context, fnReturnType, isThrowing);
                    pendingInit_ = mergePendingInit(thenPending, pendingInit_);
                } else {
                    pendingInit_ = mergePendingInit(thenPending, beforePending);
                }
            }
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
            // 确定性赋值（§1.3）：guard 的 else 必须转移控制，因此其赋值不
            // 影响后续路径，汇合时保持进入前状态。
            {
                auto beforePending = pendingInit_;
                checkStatements(g->elseBody, context, fnReturnType, isThrowing);
                pendingInit_ = beforePending;
            }
            break;
        }
        case NodeKind::LoopStmt: {
            auto* l = static_cast<LoopStmt*>(stmt);
            if (!l->label.empty()) loopLabels_.push_back(l->label);
            locals_.pushScope();
            // 确定性赋值（§1.3）：循环体可能一次都不执行（或中途 break），
            // 因此体内的赋值不能算作「所有路径都已赋值」，汇合后恢复进入前状态。
            {
                auto beforePending = pendingInit_;
                checkStatements(l->body, context, fnReturnType, isThrowing);
                pendingInit_ = beforePending;
            }
            locals_.popScope();
            if (!l->label.empty()) loopLabels_.pop_back();
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
            // 确定性赋值（§1.3）：while 体可能执行 0 次，体内赋值不计入。
            {
                auto beforePending = pendingInit_;
                checkStatements(w->body, context, fnReturnType, isThrowing);
                pendingInit_ = beforePending;
            }
            locals_.popScope();
            break;
        }
        case NodeKind::RepeatWhileStmt: {
            auto* r = static_cast<RepeatWhileStmt*>(stmt);
            locals_.pushScope();
            // 确定性赋值（§1.3）：repeat-while 至少执行一次，但仍可能在赋值前
            // break，故保守地恢复进入前状态。
            {
                auto beforePending = pendingInit_;
                checkStatements(r->body, context, fnReturnType, isThrowing);
                pendingInit_ = beforePending;
            }
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
            // Record the sequence's resolved type on the node so the code
            // generator can classify it (Array / Set / Dict / String) when it
            // emits the iteration loop. `checkExpr` does not always write this
            // back, and an Unknown here previously made `for x in set` crash.
            if (seq) f->sequence->semaType = seq;
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
            // `for await x in seq` iterates an async sequence (Channel<T> /
            // TaskGroup<T>); the loop variable takes the element type T.
            if (f->isAsync && seq && seq->kind == TypeKind::Named &&
                !seq->elements.empty()) {
                elem = seq->elements[0];
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
            // 确定性赋值（§1.3）：序列可能为空，体内赋值不计入。
            {
                auto beforePending = pendingInit_;
                checkStatements(f->body, context, fnReturnType, isThrowing);
                pendingInit_ = beforePending;
            }
            locals_.popScope();
            break;
        }
        case NodeKind::SwitchStmt: {
            auto* s = static_cast<SwitchStmt*>(stmt);
            // `select` (规范 7.5): every case is a channel operation. Typing the
            // channel here also gives a receiving case's binding the channel's
            // element type, which the code generator needs to allocate its slot.
            if (s->isSelect) {
                for (auto& c : s->cases) {
                    if (!c || c->kind != NodeKind::CaseClause) continue;
                    auto* cc = static_cast<CaseClause*>(c.get());
                    if (cc->isDefault || !cc->pattern) continue;
                    if (cc->pattern->kind != NodeKind::BinaryExpr) continue;
                    auto* be = static_cast<BinaryExpr*>(cc->pattern.get());
                    if (be->op != PunctuatorID::LeftArrow) continue;
                    const Type* chTy = checkExpr(be->rhs.get(), context);
                    const Type* elemTy =
                        (chTy && chTy->kind == TypeKind::Named &&
                         !chTy->elements.empty()) ? chTy->elements[0] : nullptr;
                    if (!elemTy) continue;
                    for (const auto& nm : cc->bindings)
                        declareLocal(*this, locals_, nm, elemTy,
                                     cc->pattern.get(), true);
                }
            }
            const Type* subj = s->subject ? checkExpr(s->subject.get(), context)
                                          : types_.unknownType();
            // Record the subject's resolved type on the node so the code
            // generator can recover the generic instantiation of an enum value
            // (e.g. `Box<Int>`) when it lowers a case pattern's payload. Without
            // this a generic enum's payload type stays unresolved (T -> ptr) and
            // the bound value is returned boxed instead of as its concrete type.
            if (subj && s->subject) s->subject->semaType = subj;
            // `case let v` needs the subject's type to type the binding.
            const Type* savedSubject = switchSubjectType_;
            switchSubjectType_ = subj;
            // 确定性赋值（§1.3）：各 case 分别在当前状态的副本上分析，未赋值
            // 集合取并集。若没有 `default`，则还存在「无 case 匹配」这条路径，
            // 需并入进入前的状态，因此只有穷举（有 default）时才算全路径已赋值。
            {
                auto beforePending = pendingInit_;
                std::unordered_set<std::string> merged;
                bool hasDefaultCase = false;
                for (auto& c : s->cases) {
                    if (c && c->kind == NodeKind::CaseClause &&
                        static_cast<CaseClause*>(c.get())->isDefault)
                        hasDefaultCase = true;
                }
                for (auto& c : s->cases) {
                    pendingInit_ = beforePending;
                    checkStatement(c.get(), context, fnReturnType, isThrowing);
                    merged = mergePendingInit(merged, pendingInit_);
                }
                pendingInit_ = hasDefaultCase ? merged
                                              : mergePendingInit(merged, beforePending);
            }
            switchSubjectType_ = savedSubject;

            // 穷举检查（规范 1.7）：带 `default` / 值绑定兜底（`case let v`）/
            // 枚举全 case 覆盖，即视为穷举，用于 missing-return 判定。
            {
                // 把枚举类型规范化到其原始声明记录：泛型枚举的实例化记录通过
                // `origRec` 回指，使主体（可能是实例化或原始）与 case 模式在覆盖
                // 比较中始终视为同一枚举。
                auto canonEnumRec = [&](const Type* t) -> const TypeRecord* {
                    if (!t || t->kind != TypeKind::Named) return nullptr;
                    if (t->record && t->record->kind == TypeDeclKind::Enum)
                        return t->record->origRec ? t->record->origRec : t->record;
                    return nullptr;
                };
                bool hasDefault = false, hasBinding = false;
                std::vector<std::string> covered;
                bool isEnum = canonEnumRec(subj) != nullptr;
                const TypeRecord* erec = isEnum ? canonEnumRec(subj) : nullptr;
                auto enumRecordOf = [&](Node* base) -> const TypeRecord* {
                    if (!base) return nullptr;
                    if (base->semaType) {
                        const TypeRecord* r = canonEnumRec(base->semaType);
                        if (r) return r;
                    }
                    // 非泛型：基类 IdentExpr 无 semaType，按名查找。
                    std::string nm;
                    if (base->kind == NodeKind::IdentExpr)
                        nm = static_cast<IdentExpr*>(base)->name;
                    else if (base->kind == NodeKind::GenericExpr) {
                        Node* inner = static_cast<GenericExpr*>(base)->base.get();
                        if (inner && inner->kind == NodeKind::IdentExpr)
                            nm = static_cast<IdentExpr*>(inner)->name;
                    }
                    if (!nm.empty())
                        if (const TypeRecord* r = findType(nm)) return r;
                    return nullptr;
                };
                auto collectEnumCase = [&](Node* p) {
                    if (!p || p->kind != NodeKind::MemberExpr) return;
                    auto* pm = static_cast<MemberExpr*>(p);
                    if (enumRecordOf(pm->base.get()) == erec)
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
                bool exhaustive = hasDefault || hasBinding;
                if (!exhaustive && erec) {
                    std::string missing;
                    for (const auto& ci : erec->cases)
                        if (std::find(covered.begin(), covered.end(), ci.name) == covered.end())
                            missing += (missing.empty() ? "" : ", ") + ci.name;
                    if (!missing.empty()) {
                        hadError_ = true;
                        diags_.reportError("switch over enum '" + erec->name +
                            "' must be exhaustive; missing case(s): " + missing,
                            s->subject ? rangeOf(s->subject.get()) : rangeOf(stmt));
                    } else {
                        exhaustive = true; // 全部 case 均已覆盖
                    }
                }
                static_cast<SwitchStmt*>(stmt)->isExhaustive = exhaustive;
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
                auto baseTypeName = [](Node* base) -> std::string {
                    if (!base) return "";
                    if (base->kind == NodeKind::IdentExpr)
                        return static_cast<IdentExpr*>(base)->name;
                    if (base->kind == NodeKind::GenericExpr) {
                        Node* inner = static_cast<GenericExpr*>(base)->base.get();
                        if (inner && inner->kind == NodeKind::IdentExpr)
                            return static_cast<IdentExpr*>(inner)->name;
                    }
                    return "";
                };
                const std::string bn = baseTypeName(pm->base.get());
                if (!bn.empty()) {
                    if (const TypeRecord* rec = findType(bn)) {
                        if (rec->kind == TypeDeclKind::Enum) {
                            for (auto& ci : rec->cases) {
                                if (ci.name != pm->member) continue;
                                for (size_t i = 0;
                                     i < c->bindings.size() && i < ci.associated.size(); ++i) {
                                    Symbol bs;
                                    bs.kind = Symbol::Kind::Variable;
                                    // 关联值可能是泛型枚举的类型参数（如 `Box<T>` 的
                                    // `T`）；按主体类型 `Box<Int>` 的实际实参替换成具体类型。
                                    const Type* bt = ci.associated[i];
                                    if (bt && bt->kind == TypeKind::Named) {
                                        auto pit = std::find(rec->genericParams.begin(),
                                                             rec->genericParams.end(),
                                                             bt->name);
                                        if (pit != rec->genericParams.end()) {
                                            size_t idx = std::distance(rec->genericParams.begin(),
                                                                       pit);
                                            const Type* subjT = switchSubjectType_;
                                            if (subjT && subjT->kind == TypeKind::Named &&
                                                idx < subjT->elements.size())
                                                bt = subjT->elements[idx];
                                        }
                                    }
                                    bs.type = bt;
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
            // 确定性赋值（§1.3）：do 体可能在中途抛错而由 catch 接手，
            // 两条路径都要考虑，汇合时未赋值集合取并集。
            {
                auto beforePending = pendingInit_;
                checkStatements(d->body, context, fnReturnType, isThrowing);
                auto bodyPending = pendingInit_;
                std::unordered_set<std::string> catchMerged;
                for (auto& c : d->catches) {
                    pendingInit_ = beforePending;
                    checkStatement(c.get(), context, fnReturnType, isThrowing);
                    catchMerged = mergePendingInit(catchMerged, pendingInit_);
                }
                pendingInit_ = d->catches.empty()
                                   ? bodyPending
                                   : mergePendingInit(bodyPending, catchMerged);
            }
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
        case NodeKind::UnsafeStmt: {
            // 进入 unsafe 上下文（规范 8.6）：裸指针/非托管类型在此可用。
            bool savedUnsafe = unsafeContext_;
            unsafeContext_ = true;
            checkStatements(static_cast<UnsafeStmt*>(stmt)->body, context, fnReturnType, isThrowing);
            unsafeContext_ = savedUnsafe;
            break;
        }
        case NodeKind::BreakStmt:
        case NodeKind::ContinueStmt: {
            // 验证标签（若有）指向真实存在的带标签循环（规范 §1.7）。
            std::string lbl = (stmt->kind == NodeKind::BreakStmt)
                                  ? static_cast<BreakStmt*>(stmt)->label
                                  : static_cast<ContinueStmt*>(stmt)->label;
            if (!lbl.empty()) {
                bool found = false;
                for (auto& x : loopLabels_)
                    if (x == lbl) { found = true; break; }
                if (!found) {
                    hadError_ = true;
                    diags_.reportError("use of undeclared label '" + lbl + "'",
                                       rangeOf(stmt));
                }
            }
            break;
        }
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
            // 穷举的 switch（枚举全 case / 有 `default` / 有 `case let v` 兜底）
            // 且每个分支都转移控制，则所有路径都转移（规范 1.7）。
            auto* sw = static_cast<SwitchStmt*>(stmt);
            if (!sw->isExhaustive) return false;
            for (auto& c : sw->cases) {
                if (!c || c->kind != NodeKind::CaseClause) continue;
                auto* cc = static_cast<CaseClause*>(c.get());
                if (!blockAlwaysTransfers(cc->body)) return false;
            }
            return true;
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

// 整数字面量范围校验（规范 §1.5）。原始文本含可选前缀（0x/0o/0b）、可选符号、
// 整数字面量范围校验（规范 §1.5）。编译器当前将所有整数字面量按 Int(i64)
// 处理、类型后缀（i8…u64）在 codegen 端被忽略，故此处以 i64 为唯一基准判
// 溢出：超界或 ERANGE 时报错，避免静默饱和成错误值（如 0xFFFFFFFFFFFFFFFF
// 被当作 -1 生成）。这是真实的非生产级安全/正确性缺陷修复。
void Sema::checkIntegerLiteral(Node* e) {
    auto* il = static_cast<IntLitExpr*>(e);
    const std::string& raw = il->value;
    size_t i = 0;
    bool neg = false;
    if (i < raw.size() && (raw[i] == '+' || raw[i] == '-')) { neg = (raw[i] == '-'); ++i; }

    // 基数前缀（0x/0b/0o）与数字部分（含下划线分隔符）。类型后缀（i8…u64）
    // 在词法上跟在数字之后，但 codegen 忽略之，故此处不收窄宽度，只以 i64 判溢出。
    int base = 10;
    size_t p = i;
    if (p + 1 < raw.size() && raw[p] == '0' &&
        (raw[p + 1] == 'x' || raw[p + 1] == 'X')) { base = 16; p += 2; }
    else if (p + 1 < raw.size() && raw[p] == '0' &&
             (raw[p + 1] == 'b' || raw[p + 1] == 'B')) { base = 2; p += 2; }
    else if (p + 1 < raw.size() && raw[p] == '0' &&
             (raw[p + 1] == 'o' || raw[p + 1] == 'O')) { base = 8; p += 2; }
    while (p < raw.size()) {
        char c = raw[p];
        bool ok = (c >= '0' && c <= '9') || c == '_';
        if (base == 16) ok = ok || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        else if (base == 2) ok = (c == '0' || c == '1' || c == '_');
        else if (base == 8) ok = (c >= '0' && c <= '7' || c == '_');
        if (!ok) break;
        ++p;
    }
    std::string digits = raw.substr(i, p - i);
    // 剥离内部下划线。
    for (char& c : digits) if (c == '_') c = ' ';
    digits.erase(std::remove(digits.begin(), digits.end(), ' '), digits.end());

    errno = 0;
    char* end = nullptr;
    unsigned long long mag = std::strtoull(digits.c_str(), &end, base);
    if (errno == ERANGE) {
        hadError_ = true;
        diags_.reportError("integer literal '" + raw + "' is out of range", rangeOf(e));
        return;
    }
    // 以 i64 为基准：有效范围为 [-2^63, 2^63-1]。
    bool overflow = neg ? (mag > 9223372036854775808ULL)
                        : (mag > 9223372036854775807ULL);
    if (overflow) {
        hadError_ = true;
        diags_.reportError("integer literal '" + raw + "' is out of range for 'Int'",
                           rangeOf(e));
    }
}

const Type* Sema::checkExprInner(Node* e, const TypeRecord* context) {
    if (!e) return types_.unknownType();
    switch (e->kind) {
        case NodeKind::IntLitExpr: {
            checkIntegerLiteral(e);
            return types_.intType();
        }
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
                // 规范 §1.3：不允许在任何路径上读取尚未确定赋值的 let。
                if (!inAssignmentLhs_ && pendingInit_.count(name)) {
                    hadError_ = true;
                    diags_.reportError("constant '" + name +
                                       "' used before being initialized", rangeOf(e));
                }
                id->semaType = s->type ? s->type : types_.unknownType();
                return id->semaType;
            }
            if (const Symbol* s = globals_.lookup(name)) {
                // 规范 §10.1：跨模块仅 public/open 可见 —— 被导入模块的
                // internal/fileprivate/private 声明不构成公开接口。
                if (!isAccessible(s->module, s->access)) {
                    hadError_ = true;
                    diags_.reportError("'" + name + "' is inaccessible: it is not "
                                       "'public' and belongs to another module",
                                       rangeOf(e));
                }
                // 顶层 `let x: Int` 同样受确定性赋值约束。
                if (!inAssignmentLhs_ && pendingInit_.count(name)) {
                    hadError_ = true;
                    diags_.reportError("constant '" + name +
                                       "' used before being initialized", rangeOf(e));
                }
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
            // `try` passes the operand type through unchanged.
            if (u->isTry) return t ? t : types_.unknownType();
            // `await` is only valid on an async call (whose type is Future<R>) or a
            // stored Future handle — never on an ordinary, non-async value.
            if (u->isAwait) {
                if (t && t->kind == TypeKind::Future)
                    return (t->element && t->element->kind != TypeKind::Unknown)
                               ? t->element : types_.voidType();
                hadError_ = true;
                diags_.reportError("'await' 只能用于 async 调用或已暂存的 Future 句柄"
                                   "（此处类型：" + typeToString(t) + "）", rangeOf(e));
                return types_.unknownType();
            }
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
        case NodeKind::AsmExpr: {
            auto* a = static_cast<AsmExpr*>(e);
            // 内联汇编仅允许在 unsafe 块内（规范 8.2 / 8.6）。
            if (!unsafeContext_) {
                hadError_ = true;
                diags_.reportError("inline assembly 'asm' may only appear inside an "
                                   "'unsafe' block", rangeOf(e));
            }
            for (auto& o : a->outputs) checkExpr(o.expr.get(), context);
            for (auto& i : a->inputs) checkExpr(i.expr.get(), context);
            // 整体类型取首个输出操作数的类型，否则 Void（通常作为语句出现）。
            if (!a->outputs.empty() && a->outputs[0].expr && a->outputs[0].expr->semaType)
                a->semaType = a->outputs[0].expr->semaType;
            else
                a->semaType = types_.voidType();
            return a->semaType;
        }
        case NodeKind::MemberExpr: {
            auto* m = static_cast<MemberExpr*>(e);
            // MemoryLayout<T>.size / .stride / .alignment（规范 P4.5）：编译期布局
            // 常量。须在 checkExpr(base) 之前识别，否则 `MemoryLayout` 作为未声明
            // 标识符会误报 "not found"。支持两种解析形态：NamedType 带泛型实参
            // （类型位置）与 GenericExpr（表达式位置）。
            {
                Node* mlBase = m->base.get();
                Node* mlArg = nullptr;
                if (mlBase && mlBase->kind == NodeKind::NamedType) {
                    auto* bnt = static_cast<NamedType*>(mlBase);
                    if (bnt->name == "MemoryLayout" && !bnt->genericArgs.empty())
                        mlArg = bnt->genericArgs[0].get();
                } else if (mlBase && mlBase->kind == NodeKind::GenericExpr) {
                    auto* ge = static_cast<GenericExpr*>(mlBase);
                    if (ge->base && ge->base->kind == NodeKind::IdentExpr &&
                        static_cast<IdentExpr*>(ge->base.get())->name == "MemoryLayout" &&
                        !ge->args.empty())
                        mlArg = ge->args[0].get();
                }
                if (mlArg && (m->member == "size" || m->member == "stride" ||
                              m->member == "alignment")) {
                    m->isMemoryLayoutQuery = true;
                    m->memoryLayoutType = resolveTypeRepr(mlArg, context);
                    m->semaType = types_.intType();
                    return m->semaType;
                }
            }
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
            // 类型化非托管指针特化（规范 §6.5 / §8.1）：`pointee` 与 `deallocate`
            // 均非普通成员，由 codegen 特化生成。`UnsafeMutablePointer<T>` /
            // `UnsafePointer<T>` 的类型名可能带泛型后缀（如 `UnsafeMutablePointer<Int>`），
            // 故按前缀匹配基础类型名。
            if (base && base->kind == TypeKind::Named && !base->elements.empty()) {
                const std::string& pn = base->name;
                auto hasPrefix = [&](const char* p) {
                    return pn == p || pn.rfind(std::string(p) + "<", 0) == 0;
                };
                if ((hasPrefix("UnsafeMutablePointer") || hasPrefix("UnsafePointer")) &&
                    m->member == "pointee") {
                    m->isUnsafePointee = true;
                    m->isUnsafePointeeMutable = hasPrefix("UnsafeMutablePointer");
                    m->semaType = base->elements[0];
                    return m->semaType;
                }
                if (hasPrefix("UnsafeMutablePointer") && m->member == "deallocate") {
                    m->isUnsafeDeallocate = true;
                    m->semaType = types_.voidType();
                    return m->semaType;
                }
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
            // 访问控制（规范 10.1）：private 成员仅在本类型内可访问。
            if (base && base->kind == TypeKind::Named && base->record) {
                if (const TypeRecord::Member* mm = lookupMember(base->record, m->member, false))
                    checkMemberAccess(base->record, mm, e);
                else if (const TypeRecord::Member* mm = lookupMethod(base->record, m->member, 0))
                    checkMemberAccess(base->record, mm, e);
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
            // 类型化缓冲指针下标（规范 §6.5 / §8.1）：编译器特化。
            // `Unsafe{Mutable}BufferPointer<T>` 的下标并非运行时调用，而是由
            // codegen 经 inttoptr(base.raw + idx*stride) + load/store 实现。
            if (base->kind == TypeKind::Named && !base->elements.empty()) {
                const std::string& pn = base->name;
                // 类型名可能带泛型实参后缀（如 `UnsafeMutableBufferPointer<Int>`），
                // 故按前缀匹配（规范 §6.5 / §8.1）。
                auto hasPrefix = [&](const char* p) {
                    return pn == p || pn.rfind(std::string(p) + "<", 0) == 0;
                };
                if (hasPrefix("UnsafeMutableBufferPointer") ||
                    hasPrefix("UnsafeBufferPointer")) {
                    s->isUnsafeBufferSubscript = true;
                    s->isUnsafeBufferMutable = hasPrefix("UnsafeMutableBufferPointer");
                    s->semaType = base->elements[0];
                    return s->semaType;
                }
            }
            if (base->kind == TypeKind::Array) return base->element;
            if (base->kind == TypeKind::Dict) return base->value;
            return types_.unknownType();
        }
        case NodeKind::GenericExpr: {
            auto* g = static_cast<GenericExpr*>(e);
            std::vector<const Type*> args;
            for (auto& a : g->args) args.push_back(resolveTypeRepr(a.get(), context));
            // A generic *type* applied to arguments in expression position — a
            // constructor call such as `Box<Int>(value:)` — resolves to that
            // monomorphised instance, so the value built has concrete fields.
            if (g->base && g->base->kind == NodeKind::IdentExpr) {
                const std::string& bname =
                    static_cast<IdentExpr*>(g->base.get())->name;
                const TypeRecord* rec = findType(bname);
                if (rec && !rec->genericParams.empty() &&
                    args.size() == rec->genericParams.size()) {
                    bool concrete = true;
                    for (const Type* a : args)
                        if (!a || a->kind == TypeKind::Unknown) { concrete = false; break; }
                    if (concrete) return monomorphiseGenericType(rec, bname, args);
                }
            }
            return checkExpr(g->base.get(), context);
        }
        case NodeKind::AssignmentExpr: {
            auto* a = static_cast<AssignmentExpr*>(e);
            // 确定性赋值（§1.3）：左值是「写入」而非「读取」，不得触发
            // 「使用前未初始化」检查，否则 `let v: Int; v = 1` 会误报。
            const bool savedLhs = inAssignmentLhs_;
            inAssignmentLhs_ = true;
            const Type* lt = checkExpr(a->lhs.get(), context);
            inAssignmentLhs_ = savedLhs;
            const Type* rt = checkExpr(a->rhs.get(), context);
            // 不可变类型化指针禁止写入（规范 §6.5）：UnsafePointer / UnsafeBufferPointer
            // 的 pointee / 下标仅可读，赋值须改用 UnsafeMutable* 变体。
            if (a->lhs) {
                if (a->lhs->kind == NodeKind::MemberExpr) {
                    auto* pm = static_cast<MemberExpr*>(a->lhs.get());
                    if (pm->isUnsafePointee && !pm->isUnsafePointeeMutable) {
                        hadError_ = true;
                        diags_.reportError(
                            "cannot assign to immutable 'pointee' of 'UnsafePointer' "
                            "(use 'UnsafeMutablePointer')", rangeOf(a->lhs.get()));
                    }
                } else if (a->lhs->kind == NodeKind::SubscriptExpr) {
                    auto* ps = static_cast<SubscriptExpr*>(a->lhs.get());
                    if (ps->isUnsafeBufferSubscript && !ps->isUnsafeBufferMutable) {
                        hadError_ = true;
                        diags_.reportError(
                            "cannot assign to immutable subscript of 'UnsafeBufferPointer' "
                            "(use 'UnsafeMutableBufferPointer')", rangeOf(a->lhs.get()));
                    }
                }
            }
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
            // `unsafeBitCast<From, To>(value)`（规范 §6.3 C 互操作）：将 value 的
            // 位模式按 To 类型重新解释。泛型实参给出源 / 目标类型，单个实参为被
            // 重解释的值——编译器特化其类型与 codegen 行为，不走常规函数查表。
            if (c->callee && c->callee->kind == NodeKind::GenericExpr) {
                auto* g = static_cast<GenericExpr*>(c->callee.get());
                if (g->base && g->base->kind == NodeKind::IdentExpr &&
                    static_cast<IdentExpr*>(g->base.get())->name == "unsafeBitCast") {
                    if (g->args.size() != 2) {
                        hadError_ = true;
                        diags_.reportError("unsafeBitCast requires exactly two type "
                                           "arguments '<From, To>'", rangeOf(c));
                        return types_.unknownType();
                    }
                    const Type* from = resolveTypeRepr(g->args[0].get(), context);
                    const Type* to   = resolveTypeRepr(g->args[1].get(), context);
                    if (c->arguments.size() != 1) {
                        hadError_ = true;
                        diags_.reportError("unsafeBitCast takes exactly one value "
                                           "argument", rangeOf(c));
                        return types_.unknownType();
                    }
                    const Type* argT = checkExpr(c->arguments[0].get(), context);
                    if (from && argT && !isIdentical(from, argT) &&
                        !isAssignable(from, argT)) {
                        hadError_ = true;
                        diags_.reportError("unsafeBitCast: argument of type '" +
                                               typeToString(argT) +
                                               "' does not match source '" +
                                               typeToString(from) + "'",
                                           rangeOf(c));
                        return types_.unknownType();
                    }
                    c->isUnsafeBitCast = true;
                    c->bitCastFrom = from;
                    c->bitCastTo = to;
                    e->semaType = to;
                    return to;
                }
            }
            // A generic type's constructor (`Box<Int>(value:)`) names the type in a
            // GenericExpr; it yields an instance of the monomorphised type rather
            // than a function's result, so the binding and later member access see
            // the concrete fields.
            if (c->callee && c->callee->kind == NodeKind::GenericExpr) {
                auto* g = static_cast<GenericExpr*>(c->callee.get());
                if (g->base && g->base->kind == NodeKind::IdentExpr) {
                    const std::string& n =
                        static_cast<IdentExpr*>(g->base.get())->name;
                    const TypeRecord* rec = findType(n);
                    if (rec && !rec->genericParams.empty() &&
                        g->args.size() == rec->genericParams.size()) {
                        std::vector<const Type*> args;
                        for (auto& a : g->args)
                            args.push_back(resolveTypeRepr(a.get(), context));
                        bool concrete = true;
                        for (const Type* a : args)
                            if (!a || a->kind == TypeKind::Unknown) { concrete = false; break; }
                        if (concrete) {
                            for (auto& a : c->arguments) checkExpr(a.get(), context);
                            const Type* instTy = monomorphiseGenericType(rec, n, args);
                            // Annotate the callee so code generation can resolve the
                            // instance by name and emit its initialiser.
                            if (c->callee) c->callee->semaType = instTy;
                            return instTy;
                        }
                    }
                }
            }
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
                // 枚举 case 构造：支持 `E.case(...)`（非泛型）与 `E<Int>.case(...)`
                // （泛型枚举）。泛型时返回单态化实例类型，使 `let b = E<Int>.case(x)`
                // 的推断类型即实例类型。
                const TypeRecord* rec = nullptr;
                const Type* recType = nullptr;
                if (m->base) {
                    // Resolve the enum base to its (possibly monomorphised) type so
                    // the constructed value carries `Box<Int>` rather than the
                    // generic `Box`. Three spellings exist:
                    //   * `E<Int>.case(...)`  — GenericExpr, explicit args.
                    //   * `E.case(...)`        — IdentExpr, generic args inferred
                    //                             from the payload arguments.
                    //   * `E` (non-generic)    — plain named type.
                    const Type* bt = nullptr;
                    if (m->base->kind == NodeKind::GenericExpr) {
                        auto* g = static_cast<GenericExpr*>(m->base.get());
                        if (g->base && g->base->kind == NodeKind::IdentExpr) {
                            const std::string& bn =
                                static_cast<IdentExpr*>(g->base.get())->name;
                            const TypeRecord* gr = findType(bn);
                            if (gr && !gr->genericParams.empty() &&
                                g->args.size() == gr->genericParams.size()) {
                                std::vector<const Type*> args;
                                for (auto& a : g->args)
                                    args.push_back(resolveTypeRepr(a.get(), context));
                                bool concrete = true;
                                for (const Type* a : args)
                                    if (!a || a->kind == TypeKind::Unknown)
                                        { concrete = false; break; }
                                if (concrete)
                                    bt = monomorphiseGenericType(gr, bn, args);
                            }
                        }
                    } else if (m->base->kind == NodeKind::NamedType) {
                        bt = resolveTypeRepr(m->base.get(), context);
                    } else if (m->base->kind == NodeKind::IdentExpr) {
                        const std::string& bn = static_cast<IdentExpr*>(m->base.get())->name;
                        const TypeRecord* gr = findType(bn);
                        if (gr && !gr->genericParams.empty() && !inPattern_) {
                            // `Box.wrapped(5)` — no explicit `<Int>`. Infer each
                            // generic parameter from the payload argument that
                            // corresponds to the case's associated type mentioning
                            // it (e.g. `T` at position 0 ↔ argument `5` : Int).
                            // Skipped inside a pattern (the "arguments" there are
                            // bindings, not values) to avoid type-checking them.
                            const EnumCaseDecl* ecd = nullptr;
                            if (gr->decl && gr->decl->kind == NodeKind::EnumDecl) {
                                auto* ed = static_cast<EnumDecl*>(gr->decl);
                                for (auto& mm : ed->members) {
                                    if (mm && mm->kind == NodeKind::EnumCaseDecl &&
                                        static_cast<EnumCaseDecl*>(mm.get())->name == m->member) {
                                        ecd = static_cast<EnumCaseDecl*>(mm.get());
                                        break;
                                    }
                                }
                            }
                            std::vector<const Type*> args(gr->genericParams.size());
                            bool ok = true;
                            for (size_t gi = 0; gi < gr->genericParams.size(); ++gi) {
                                const std::string& gp = gr->genericParams[gi];
                                bool found = false;
                                if (ecd) {
                                    for (size_t p = 0; p < ecd->associatedTypes.size(); ++p) {
                                        if (ecd->associatedTypes[p] &&
                                            ecd->associatedTypes[p]->kind == NodeKind::NamedType &&
                                            static_cast<NamedType*>(ecd->associatedTypes[p].get())->name == gp) {
                                            if (p < c->arguments.size()) {
                                                args[gi] = inferTypeArgument(
                                                    checkExpr(c->arguments[p].get(), context));
                                                found = true;
                                                break;
                                            }
                                        }
                                    }
                                }
                                if (!found) { ok = false; break; }
                            }
                            if (ok) bt = monomorphiseGenericType(gr, bn, args);
                        } else if (gr) {
                            bt = types_.named(gr, gr->name);
                        }
                    }
                    if (!bt || bt->kind != TypeKind::Named)
                        bt = checkExpr(m->base.get(), context);
                    if (bt && bt->kind == TypeKind::Named && bt->record) {
                        rec = bt->record;
                        recType = bt;
                    }
                }
                if (rec && rec->kind == TypeDeclKind::Enum) {
                    // 必须先对基类做类型检查，设置 `m->base->semaType`——
                    // codegen 的 enumCaseOf 依赖它定位枚举记录与 case（否则构造
                    // 表达式不会生成）。
                    checkExpr(m->base.get(), context);
                    c->callee->semaType = recType;
                    m->semaType = recType;
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
                        return recType;
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
                        !fs->function->genericParams.empty()) {
                        // Infer one type argument per generic parameter. A type
                        // parameter may be used in several value parameters and the
                        // number of type parameters need not equal the number of
                        // arguments (e.g. `unwrapOr<T>(_ o: T?, fallback: T)` has
                        // one type parameter but two arguments, and `id<T>(_ x: T)`
                        // has one of each). We therefore scan each function
                        // parameter's declared type to learn which argument
                        // constrains which type variable, then read the concrete
                        // type off that argument.
                        auto isIdentChar = [](char c) {
                            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                   (c >= '0' && c <= '9') || c == '_';
                        };
                        auto mentions = [&](const std::string& src,
                                            const std::string& gp) -> bool {
                            if (src.empty()) return false;
                            size_t pos = 0;
                            while ((pos = src.find(gp, pos)) != std::string::npos) {
                                bool beforeOk = (pos == 0) || !isIdentChar(src[pos - 1]);
                                size_t after = pos + gp.size();
                                bool afterOk = (after >= src.size()) ||
                                               !isIdentChar(src[after]);
                                if (beforeOk && afterOk) return true;
                                pos += gp.size();
                            }
                            return false;
                        };
                        // Infer the generic type arguments by unifying each
                        // declared parameter type (a TypeRepr that may mention the
                        // type variables, possibly nested inside `Array` /
                        // `FuncType` / `...`) against the corresponding *actual*
                        // argument type. Closure arguments are first given the
                        // parameter types they receive, then re-checked so their
                        // body — and therefore their return type — is fully typed;
                        // that lets the remaining variables (e.g. `U` in `(T) -> U`)
                        // be bound from the callback. This is what makes `map` /
                        // `filter` / `reduce` / `forEach` work generically with
                        // `(_ x: T) -> U` callbacks (规范 3.3/3.4).
                        std::vector<std::string> targs;
                        std::vector<const Type*> tys;
                        targs.resize(fs->function->genericParams.size());
                        tys.resize(fs->function->genericParams.size());
                        std::unordered_map<std::string, const Type*> bind;
                        std::vector<Node*> argNodes;
                        for (auto& a : c->arguments) argNodes.push_back(a.get());

                        bool progress = true;
                        int iterGuard = 0;
                        while (progress && iterGuard++ < 8) {
                            progress = false;
                            for (size_t p = 0;
                                 p < fs->function->params.size() && p < argNodes.size();
                                 ++p) {
                                const Type* argT = argTypes[p];
                                Node* argNode = argNodes[p];
                                // Resolve the declared parameter type under the
                                // bindings gathered so far, to learn the concrete
                                // types a closure argument should receive.
                                std::unordered_map<std::string, const Type*> saved =
                                    genericBindings_;
                                for (auto& kv : bind)
                                    genericBindings_[kv.first] = kv.second;
                                const Type* pT = fs->function->params[p].type
                                    ? resolveTypeRepr(
                                          fs->function->params[p].type.get(), nullptr)
                                    : types_.unknownType();
                                genericBindings_ = saved;
                                if (!pT) continue;
                                if (pT->kind == TypeKind::Function ||
                                    pT->kind == TypeKind::Closure) {
                                    // The argument is (or should become) a closure.
                                    if (argNode &&
                                        argNode->kind == NodeKind::ClosureExpr &&
                                        !hadError_) {
                                        auto* cl = static_cast<ClosureExpr*>(argNode);
                                        // Seed the closure's parameter types once we
                                        // know them concretely, then re-check so its
                                        // return type is inferred (needed to bind the
                                        // remaining type variables). `hadError_` guards
                                        // against re-reporting an already-flagged body.
                                        bool seedUnknown =
                                            cl->semaType == nullptr ||
                                            cl->semaType->elements.empty() ||
                                            cl->semaType->elements[0]->kind ==
                                                TypeKind::Unknown;
                                        if (seedUnknown) {
                                            for (size_t k = 0;
                                                 k < cl->params.size() &&
                                                 k < pT->elements.size(); ++k) {
                                                const Type* pt = pT->elements[k];
                                                if (pt && pt->kind != TypeKind::Unknown)
                                                    cl->params[k].semaType = pt;
                                            }
                                            const Type* clT = checkExpr(argNode, context);
                                            if (clT) argT = clT;
                                        }
                                        argTypes[p] = argT;
                                    }
                                    if (unifyGenericParamNode(
                                            fs->function->params[p].type.get(), argT,
                                            fs->function->genericParams, bind))
                                        progress = true;
                                    continue;
                                }
                                if (unifyGenericParamNode(
                                        fs->function->params[p].type.get(), argT,
                                        fs->function->genericParams, bind))
                                    progress = true;
                            }
                        }

                        // Promote the solved bindings into targs/tys.
                        for (size_t i = 0; i < fs->function->genericParams.size(); ++i) {
                            const std::string& gp = fs->function->genericParams[i];
                            auto it = bind.find(gp);
                            if (it != bind.end() && it->second &&
                                it->second->kind != TypeKind::Unknown) {
                                tys[i] = it->second;
                                targs[i] = typeToString(it->second);
                            }
                        }
                        // Fallback for type variables that appear *only* in the return
                        // type (e.g. `allocate<T>(capacity: Int) -> UnsafeMutablePointer<T>`):
                        // unifying against the value arguments cannot see them, so reuse the
                        // original heuristic for the remaining variables to avoid a regression.
                        for (size_t gi = 0; gi < fs->function->genericParams.size(); ++gi) {
                            const std::string& gp = fs->function->genericParams[gi];
                            if (bind.count(gp) && bind[gp] &&
                                bind[gp]->kind != TypeKind::Unknown) continue;
                            bool found = false;
                            for (size_t p = 0;
                                 p < fs->function->params.size() && p < argTypes.size(); ++p) {
                                if (mentions(typeToSource(fs->function->params[p].type.get()),
                                             gp)) {
                                    const Type* base = inferTypeArgument(argTypes[p]);
                                    if (base && base->kind != TypeKind::Unknown) {
                                        bind[gp] = base;
                                        tys[gi] = base;
                                        targs[gi] = typeToString(base);
                                        found = true;
                                        break;
                                    }
                                }
                            }
                            if (!found) {
                                const Type* base = inferTypeArgument(
                                    argTypes.empty() ? types_.unknownType() : argTypes[0]);
                                if (base && base->kind != TypeKind::Unknown) {
                                    bind[gp] = base;
                                    tys[gi] = base;
                                    targs[gi] = typeToString(base);
                                }
                            }
                        }
                        bool inferOk = !targs.empty();
                        for (const auto& t : tys)
                            if (!t || t->kind == TypeKind::Unknown) { inferOk = false; break; }
                        if (inferOk) {
                            recordGenericInstance(fname, targs);
                            // Keep the resolved type arguments on the call so codegen
                            // can locate the exact monomorphised instance (规范 5.2).
                            c->genericTypeArgs = targs;
                            // Resolve the call's result + parameter types against
                            // this instantiation, so `let n = f(x)` and the closure
                            // body both learn the concrete types.
                            std::unordered_map<std::string, const Type*> saved =
                                genericBindings_;
                            for (size_t i = 0; i < targs.size(); ++i)
                                genericBindings_[fs->function->genericParams[i]] = tys[i];
                            std::vector<const Type*> resolvedParams;
                            resolvedParams.reserve(fs->function->params.size());
                            for (auto& prm : fs->function->params) {
                                const Type* rt = prm.type
                                    ? resolveTypeRepr(prm.type.get(), nullptr)
                                    : types_.unknownType();
                                resolvedParams.push_back(rt);
                            }
                            const Type* ret = fs->function->returnType
                                ? resolveTypeRepr(fs->function->returnType.get(), nullptr)
                                : types_.voidType();
                            genericBindings_ = saved;
                            callee = types_.function(resolvedParams, ret);
                        }
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
                // An `async` call yields a Future of its result type; `await` later
                // unwraps that handle back to the value.
                if (fdecl && fdecl->isAsync)
                    return types_.future(callee->ret ? callee->ret
                                                     : types_.voidType());
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

// ─────────────────────────────────────────────────────────────────────────────
// 宏展开（规范 5.6）
//
// 设计要点：
//   * 宏展开在 parse 之后、Sema 类型解析之前完成；展开产物是普通 AST，复用现有
//     Sema / codegen，不污染后端。
//   * freestanding(expression) 宏体形如 `return #makeExpr("…\(x)…")`。模板字符串
//     经普通字符串插值解析后，其 `\(x)` 变为 StrLitExpr 的 interpolation 表达式
//     （此处为引用宏参数 x 的 IdentExpr）。展开时把每个 `\(x)` 替换为对应实参的
//     *原始源代码文本*（由节点的 SourceRange.offset 在 source_ 中截取），再整体
//     重新解析为表达式，就地替换调用点的 CallExpr。
//   * 卫生性（hygiene）：展开体内新引入的绑定名按规范加 `__suki_macro_<scopeID>_`
//     前缀；unquote 嵌入的节点保留调用方作用域（其文本来自调用点，天然保留作用域）。
//     M1 仅实现表达式宏，通常无新绑定，故基础重命名已足够；`#unique` 生成保证唯一
//     的标识符。声明位宏（@freestanding(declaration)）与 @attached 留待 M2。
// ─────────────────────────────────────────────────────────────────────────────

void Sema::expandMacros(NodeList& decls) {
    // 1. 注册全部宏声明。
    for (auto& d : decls)
        if (d && d->kind == NodeKind::MacroDecl)
            collectMacro(static_cast<MacroDecl*>(d.get()));
    // 1.5 @attached(member) 宏：把展开产物拼接到被注解类型的成员列表（M2）。
    expandBudget_ = kMaxMacroComplexity_;
    for (auto& d : decls) {
        if (!d) continue;
        switch (d->kind) {
        case NodeKind::StructDecl: case NodeKind::EnumDecl:
        case NodeKind::ClassDecl:  case NodeKind::ActorDecl:
        case NodeKind::ProtocolDecl: case NodeKind::ExtensionDecl:
            break;
        default:
            continue;
        }
        auto* td = static_cast<TypeDecl*>(d.get());
        for (const auto& attr : td->attributes) {
            auto it = macros_.find(attr);
            if (it == macros_.end()) continue;
            MacroDecl* md = it->second;
            if (md->role == "attached" && md->kind == "member" && md->expansionKind_ == "decl") {
                NodeList injected = tryExpandAttached(md, td->name);
                for (auto& m : injected) td->members.push_back(std::move(m));
            }
        }
    }
    // 2. 递归展开（表达式位宏调用 + 声明位宏调用 + attached 注入成员的嵌套宏）。
    expandDeclList(decls);
    // 3. 宏声明为编译期构造，展开后即可从顶层移除，避免进入后续分析与 codegen。
    decls.erase(std::remove_if(decls.begin(), decls.end(),
                   [](const NodePtr& d) { return d && d->kind == NodeKind::MacroDecl; }),
                decls.end());
}

void Sema::collectMacro(MacroDecl* md) {
    macros_[md->name] = md;
    // 提前抽取模板（确定 expansionKind_），使下方 gate 判断（role/kind）
    // 与展开逻辑可立即使用，无需等到第一次展开调用。
    if (!md->expansionExtracted && !md->extractionFailed)
        extractTemplate(md);
}

bool Sema::extractTemplate(MacroDecl* md) {
    md->expansionExtracted = true;
    // 寻找 `return #makeExpr("…") / #makeDecl("…")`。
    for (auto& st : md->body) {
        if (!st || st->kind != NodeKind::ReturnStmt) continue;
        Node* e = static_cast<ReturnStmt*>(st.get())->value.get();
        if (!e) continue;
        if (e->kind == NodeKind::ExprStmt)
            e = static_cast<ExprStmt*>(e)->expr.get();
        if (e->kind != NodeKind::CallExpr) continue;
        auto* ce = static_cast<CallExpr*>(e);
        if (!ce->callee || ce->callee->kind != NodeKind::IdentExpr) continue;
        std::string callee = static_cast<IdentExpr*>(ce->callee.get())->name;
        bool isMake = (callee == "#makeExpr" || callee == "#makeDecl");
        if (!isMake || ce->arguments.empty()) continue;
        md->expansionKind_ = (callee == "#makeDecl") ? "decl" : "expr";
        Node* arg = ce->arguments[0].get();
        if (!arg || arg->kind != NodeKind::StrLitExpr) return false;
        auto* sl = static_cast<StrLitExpr*>(arg);
        // StrLitExpr 约定：segments.size() == expressions.size() + 1。
        md->templateSegments_ = sl->segments;
        md->unquoteNames_.clear();
        md->unquoteNames_.reserve(sl->expressions.size());
        for (auto& ex : sl->expressions) {
            if (ex && ex->kind == NodeKind::IdentExpr)
                md->unquoteNames_.push_back(static_cast<IdentExpr*>(ex.get())->name);
            else
                md->unquoteNames_.push_back(std::string()); // 暂不支持的 unquote 形式
        }
        return true;
    }
    return false;
}

// ── AST→源码递归序列化（宏 unquote 实参还原） ────────────────────────────────
// Parser 当前把所有节点的 range 都设成零长度单点，无法用区间截取源码，
// 故采用递归序列化：把任意表达式/类型 AST 还原成与其源码等价的文本。
bool Sema::macroArgNeedsParens_(Node* n) const {
    if (!n) return false;
    switch (n->kind) {
    case NodeKind::BinaryExpr:
    case NodeKind::UnaryExpr:       // 前缀运算符（如 -a）需保护
    case NodeKind::TernaryExpr:
    case NodeKind::RangeExpr:
    case NodeKind::AsExpr:
    case NodeKind::IsExpr:
    case NodeKind::AssignmentExpr:
    case NodeKind::MoveExpr:
        return true;
    default:
        return false;
    }
}

std::string Sema::exprToSource(Node* n) const {
    std::string s;
    if (!n) return s;
    switch (n->kind) {
    case NodeKind::IdentExpr:
        s += static_cast<IdentExpr*>(n)->name; break;
    case NodeKind::IntLitExpr:
    case NodeKind::FloatLitExpr:
    case NodeKind::CharLitExpr:
        if (n->kind == NodeKind::IntLitExpr)      s += static_cast<IntLitExpr*>(n)->value;
        else if (n->kind == NodeKind::FloatLitExpr) s += static_cast<FloatLitExpr*>(n)->value;
        else                                       s += static_cast<CharLitExpr*>(n)->value;
        break;
    case NodeKind::BoolLitExpr:
        s += static_cast<BoolLitExpr*>(n)->value ? "true" : "false"; break;
    case NodeKind::NilLitExpr:
        s += "nil"; break;
    case NodeKind::StrLitExpr: {
        auto* sl = static_cast<StrLitExpr*>(n);
        s += sl->isRaw ? "#\"" : "\"";
        for (const auto& seg : sl->segments) s += seg;
        s += '"'; break;
    }
    case NodeKind::BinaryExpr: {
        auto* e = static_cast<BinaryExpr*>(n);
        s += exprToSource(e->lhs.get());
        s += ' '; s += punctToString(e->op); s += ' ';
        s += exprToSource(e->rhs.get()); break;
    }
    case NodeKind::UnaryExpr: {
        auto* e = static_cast<UnaryExpr*>(n);
        if (e->isPostfix) { s += exprToSource(e->operand.get()); s += punctToString(e->op); }
        else              { s += punctToString(e->op); s += exprToSource(e->operand.get()); }
        break;
    }
    case NodeKind::CallExpr: {
        auto* e = static_cast<CallExpr*>(n);
        s += exprToSource(e->callee.get());
        s += '(';
        for (size_t i = 0; i < e->arguments.size(); ++i) {
            if (i) s += ", ";
            if (i < e->argumentLabels.size() && !e->argumentLabels[i].empty()) {
                s += e->argumentLabels[i]; s += ": ";
            }
            s += exprToSource(e->arguments[i].get());
        }
        s += ')'; break;
    }
    case NodeKind::MemberExpr: {
        auto* e = static_cast<MemberExpr*>(n);
        s += exprToSource(e->base.get());
        if (e->optionalChain) s += '?';
        s += '.'; s += e->member; break;
    }
    case NodeKind::SubscriptExpr: {
        auto* e = static_cast<SubscriptExpr*>(n);
        s += exprToSource(e->base.get());
        s += '[';
        for (size_t i = 0; i < e->indices.size(); ++i) {
            if (i) s += ", ";
            s += exprToSource(e->indices[i].get());
        }
        s += ']'; break;
    }
    case NodeKind::ParenExpr:
        s += '('; s += exprToSource(static_cast<ParenExpr*>(n)->expr.get()); s += ')'; break;
    case NodeKind::AsExpr: {
        auto* e = static_cast<AsExpr*>(n);
        s += exprToSource(e->expr.get());
        s += (e->asKind == AsExpr::AsBang ? " as! " :
              e->asKind == AsExpr::AsQuestion ? " as? " : " as ");
        s += typeToSource(e->type.get()); break;
    }
    case NodeKind::IsExpr: {
        auto* e = static_cast<IsExpr*>(n);
        s += exprToSource(e->expr.get());
        s += " is "; s += typeToSource(e->type.get()); break;
    }
    case NodeKind::RangeExpr: {
        auto* e = static_cast<RangeExpr*>(n);
        if (e->lower) s += exprToSource(e->lower.get());
        s += e->halfOpen ? "..<" : "...";
        if (e->upper) s += exprToSource(e->upper.get());
        break;
    }
    case NodeKind::TernaryExpr: {
        auto* e = static_cast<TernaryExpr*>(n);
        s += exprToSource(e->condition.get()); s += " ? ";
        s += exprToSource(e->thenValue.get());  s += " : ";
        s += exprToSource(e->elseValue.get()); break;
    }
    case NodeKind::AssignmentExpr: {
        auto* e = static_cast<AssignmentExpr*>(n);
        s += exprToSource(e->lhs.get());
        if (e->isCompound) { s += ' '; s += punctToString(e->compoundOp); s += ' '; }
        else               { s += " = "; }
        s += exprToSource(e->rhs.get()); break;
    }
    case NodeKind::ArrayLitExpr: {
        auto* e = static_cast<ArrayLitExpr*>(n);
        s += '[';
        for (size_t i = 0; i < e->elements.size(); ++i) {
            if (i) s += ", ";
            s += exprToSource(e->elements[i].get());
        }
        s += ']'; break;
    }
    case NodeKind::TupleExpr: {
        auto* e = static_cast<TupleExpr*>(n);
        s += '(';
        for (size_t i = 0; i < e->elements.size(); ++i) {
            if (i) s += ", ";
            if (i < e->labels.size() && !e->labels[i].empty()) { s += e->labels[i]; s += ": "; }
            s += exprToSource(e->elements[i].get());
        }
        s += ')'; break;
    }
    case NodeKind::OptionalChainExpr:
        s += exprToSource(static_cast<OptionalChainExpr*>(n)->expr.get()); break;
    case NodeKind::ForceUnwrapExpr: {
        auto* e = static_cast<ForceUnwrapExpr*>(n);
        s += exprToSource(e->expr.get()); s += '!'; break;
    }
    case NodeKind::MoveExpr:
        s += "move "; s += exprToSource(static_cast<MoveExpr*>(n)->operand.get()); break;
    case NodeKind::ClosureExpr:
        s += "{ /*closure*/ }"; break;  // M1：宏实参极少为闭包，占位即可
    default:
        break;
    }
    return s;
}

std::string Sema::typeToSource(Node* n) const {
    if (!n) return std::string();
    switch (n->kind) {
    case NodeKind::NamedType: {
        auto* t = static_cast<NamedType*>(n);
        std::string s = t->name;
        if (!t->genericArgs.empty()) {
            s += '<';
            for (size_t i = 0; i < t->genericArgs.size(); ++i) {
                if (i) s += ", ";
                s += typeToSource(t->genericArgs[i].get());
            }
            s += '>';
        }
        return s;
    }
    case NodeKind::OptionalType:
        return typeToSource(static_cast<OptionalType*>(n)->wrapped.get()) + "?";
    case NodeKind::ArrayType:
        return "[" + typeToSource(static_cast<ArrayType*>(n)->element.get()) + "]";
    case NodeKind::DictType: {
        auto* t = static_cast<DictType*>(n);
        return "[" + typeToSource(t->key.get()) + ": " + typeToSource(t->value.get()) + "]";
    }
    case NodeKind::TupleType: {
        auto* t = static_cast<TupleType*>(n);
        std::string s = "(";
        for (size_t i = 0; i < t->elements.size(); ++i) {
            if (i) s += ", ";
            s += typeToSource(t->elements[i].get());
        }
        return s + ")";
    }
    case NodeKind::RefType:
        return static_cast<RefType*>(n)->refKind + " " +
               typeToSource(static_cast<RefType*>(n)->pointee.get());
    case NodeKind::InoutType:
        return "inout " + typeToSource(static_cast<InoutType*>(n)->pointee.get());
    default:
        return std::string();
    }
}

// 由模板 + 实参构造展开源码字符串（表达式宏与声明宏共用）。
// selfType 非空时允许模板内 \(Self) 引用被注解类型的名字（@attached 用）。
std::string Sema::buildMacroCode(MacroDecl* md, CallExpr* call, const std::string& selfType) {
    if (!md->expansionExtracted && !extractTemplate(md)) {
        if (!md->extractionFailed) {
            md->extractionFailed = true;
            diags_.reportError("macro '" + md->name +
                "' body must be 'return #makeExpr(\"...\")' or '#makeDecl(\"...\")'",
                call->range);
        }
        return std::string();
    }
    if (md->templateSegments_.size() != md->unquoteNames_.size() + 1)
        return std::string();
    // 参数名 → 实参节点（按宏参数定义顺序映射）。
    std::unordered_map<std::string, Node*> argOf;
    for (size_t i = 0; i < md->params.size() && i < call->arguments.size(); ++i) {
        const std::string& pname = md->params[i].internalName.empty()
            ? md->params[i].externalName : md->params[i].internalName;
        argOf[pname] = call->arguments[i].get();
    }
    std::string code;
    for (size_t i = 0; i < md->templateSegments_.size(); ++i) {
        code += md->templateSegments_[i];
        if (i < md->unquoteNames_.size()) {
            const std::string& p = md->unquoteNames_[i];
            if (!selfType.empty() && (p == "Self" || p == "self")) {
                code += selfType;
            } else if (!p.empty() && p.front() == '#') {
                // 编译期原语：#unique 生成卫生性唯一标识符。
                if (p == "#unique")
                    code += "__suki_unique_" + std::to_string(++uniqueCounter_);
                else {
                    diags_.reportError("unknown macro primitive '" + p + "'", call->range);
                    code += p;
                }
            } else {
                auto ait = argOf.find(p);
                if (ait == argOf.end() || !ait->second) {
                    diags_.reportError("macro unquote '\\(" + p +
                        ")' has no matching argument", call->range);
                    return std::string();
                }
                std::string argSrc = exprToSource(ait->second);
                // 含运算符的实参需整体加括号，避免被模板中的运算符抢占优先级
                // （如 (3 + 4) * 2 不能写成 3 + 4 * 2）。
                if (macroArgNeedsParens_(ait->second))
                    code += "(" + argSrc + ")";
                else
                    code += argSrc;
            }
        }
    }
    return code;
}

// 把一段模板源码重新解析为声明列表（@freestanding(declaration) / @attached(member) 共用）。
NodeList Sema::expandDeclTemplate(MacroDecl* md, CallExpr* call, const std::string& selfType) {
    NodeList out;
    std::string code = buildMacroCode(md, call, selfType);
    if (code.empty()) return out;
    DiagnosticEngine subDiags;
    Lexer lexer(code, subDiags);
    auto toks = lexer.tokenizeAll();
    Parser parser(std::move(toks), subDiags);
    NodeList decls = parser.parseModule();
    if (subDiags.hasErrors() || decls.empty()) {
        std::string sub = subDiags.lastErrorMessage();
        diags_.reportError("macro '" + md->name + "' produced invalid declaration expansion" +
            (sub.empty() ? std::string("") : (": " + sub)), call->range);
        return out;
    }
    // MacroExpansionTooComplex：节点数预算。
    if (expandBudget_ > 0) {
        long n = 0;
        for (auto& d : decls) n += (long)countNodes(d.get());
        if (expandBudget_ < n) {
            diags_.reportError("macro expansion exceeded MacroExpansionTooComplex limit", call->range);
            return out;
        }
        expandBudget_ -= n;
    }
    for (auto& d : decls) {
        if (expandDepth_ < 64) { ++expandDepth_; expandInNode(d); --expandDepth_; }
        expandDeclListContainers(d.get());
    }
    return decls;
}

// @freestanding(declaration) 宏：调用点 #name(...) 展开为声明列表。
NodeList Sema::tryExpandDeclMacro(CallExpr* call, const std::string& selfType) {
    NodeList out;
    if (!call->callee || call->callee->kind != NodeKind::IdentExpr) return out;
    std::string name = static_cast<IdentExpr*>(call->callee.get())->name;
    if (name.empty() || name.front() != '#') return out;
    name = name.substr(1);
    auto it = macros_.find(name);
    if (it == macros_.end()) return out;
    MacroDecl* md = it->second;
    if (md->role != "freestanding" || md->expansionKind_ != "decl") return out;
    return expandDeclTemplate(md, call, selfType);
}

// @attached(member) 宏：被注解类型无显式实参，用合成空 CallExpr 触发模板展开。
NodeList Sema::tryExpandAttached(MacroDecl* md, const std::string& selfType) {
    if (md->expansionKind_ != "decl") return NodeList();
    CallExpr synth;
    return expandDeclTemplate(md, &synth, selfType);
}

// 判断节点是否为声明位宏调用（ExprStmt -> CallExpr(#name)，且 name 指向已注册的
// freestanding(declaration) 宏）。
bool Sema::isDeclMacroCall(Node* n) const {
    if (!n || n->kind != NodeKind::ExprStmt) return false;
    auto* es = static_cast<ExprStmt*>(n);
    if (!es->expr || es->expr->kind != NodeKind::CallExpr) return false;
    auto* ce = static_cast<CallExpr*>(es->expr.get());
    if (!ce->callee || ce->callee->kind != NodeKind::IdentExpr) return false;
    std::string name = static_cast<IdentExpr*>(ce->callee.get())->name;
    if (name.empty() || name.front() != '#') return false;
    auto it = macros_.find(name.substr(1));
    if (it == macros_.end()) return false;
    MacroDecl* md = it->second;
    return md->role == "freestanding" && md->kind == "declaration";
}

// 统计 AST 节点总数（用于 MacroExpansionTooComplex 复杂度预算）。
size_t Sema::countNodes(Node* n) {
    if (!n) return 0;
    size_t c = 1;
    walkChildren(n, [&](NodePtr& child) { c += countNodes(child.get()); });
    return c;
}

// 列表级展开：把列表里声明位宏调用（ExprStmt 承载的 #name(...)）就地替换为
// 展开得到的声明列表，并递归处理嵌套的语句块/类型成员中的声明宏。
void Sema::expandDeclList(NodeList& list) {
    NodeList out;
    for (auto& item : list) {
        if (!item) { out.push_back(nullptr); continue; }
        expandInNode(item); // 表达式位宏（M1）就地替换
        if (isDeclMacroCall(item.get())) {
            auto* es = static_cast<ExprStmt*>(item.get());
            NodeList ex = tryExpandDeclMacro(static_cast<CallExpr*>(es->expr.get()), std::string());
            for (auto& d : ex) out.push_back(std::move(d));
        } else {
            expandDeclListContainers(item.get());
            out.push_back(std::move(item));
        }
    }
    list = std::move(out);
}

// 对单个节点的「含声明列表」子容器递归调用 expandDeclList。
void Sema::expandDeclListContainers(Node* n) {
    if (!n) return;
    auto each = [&](NodeList& l) { expandDeclList(l); };
    switch (n->kind) {
    case NodeKind::BlockStmt: each(static_cast<BlockStmt*>(n)->statements); break;
    case NodeKind::IfStmt: {
        auto* s = static_cast<IfStmt*>(n);
        each(s->thenBody);
        if (s->elseBranch) expandDeclListContainers(s->elseBranch.get());
        break;
    }
    case NodeKind::GuardStmt: each(static_cast<GuardStmt*>(n)->elseBody); break;
    case NodeKind::WhileStmt: each(static_cast<WhileStmt*>(n)->body); break;
    case NodeKind::LoopStmt: each(static_cast<LoopStmt*>(n)->body); break;
    case NodeKind::RepeatWhileStmt: each(static_cast<RepeatWhileStmt*>(n)->body); break;
    case NodeKind::ForInStmt: each(static_cast<ForInStmt*>(n)->body); break;
    case NodeKind::DoStmt: each(static_cast<DoStmt*>(n)->body); break;
    case NodeKind::SwitchStmt:
        for (auto& c : static_cast<SwitchStmt*>(n)->cases)
            each(static_cast<CaseClause*>(c.get())->body);
        break;
    case NodeKind::StructDecl: case NodeKind::EnumDecl:
    case NodeKind::ClassDecl:  case NodeKind::ActorDecl:
    case NodeKind::ProtocolDecl: case NodeKind::ExtensionDecl:
        each(static_cast<TypeDecl*>(n)->members); break;
    case NodeKind::ClosureExpr: each(static_cast<ClosureExpr*>(n)->body); break;
    default: break;
    }
}

NodePtr Sema::tryExpandMacroCall(CallExpr* call, const std::string& macroName) {
    auto it = macros_.find(macroName);
    if (it == macros_.end()) return nullptr;
    MacroDecl* md = it->second;
    if (md->role != "freestanding" || md->kind != "expression")
        return nullptr; // M1：仅 freestanding(expression) 实际展开
    std::string code = buildMacroCode(md, call, std::string());
    if (code.empty()) return nullptr;
    // 重新解析为表达式。
    DiagnosticEngine subDiags;
    Lexer lexer(code, subDiags);
    auto toks = lexer.tokenizeAll();
    Parser parser(std::move(toks), subDiags);
    NodePtr result = parser.parseExpression();
    if (!result || subDiags.hasErrors()) {
        std::string sub = subDiags.lastErrorMessage();
        diags_.reportError("macro '" + md->name + "' produced invalid expansion" +
            (sub.empty() ? std::string("") : (": " + sub)), call->range);
        return nullptr;
    }
    // 嵌套展开（带深度上限防自引用死循环）。
    if (expandBudget_ > 0) {
        long n = (long)countNodes(result.get());
        if (expandBudget_ < n) {
            diags_.reportError("macro expansion exceeded MacroExpansionTooComplex limit", call->range);
            return nullptr;
        }
        expandBudget_ -= n;
    }
    if (expandDepth_ < 64) {
        ++expandDepth_;
        expandInNode(result);
        --expandDepth_;
    }
    return result;
}

void Sema::expandInNode(NodePtr& n) {
    if (!n) return;
    // 宏 *定义* 的体是模板，不应被就地展开。
    if (n->kind != NodeKind::MacroDecl)
        walkChildren(n.get(), [this](NodePtr& c) { expandInNode(c); });
    // 宏调用替换：CallExpr 且 callee 为 "#name"。
    if (n->kind == NodeKind::CallExpr) {
        auto* ce = static_cast<CallExpr*>(n.get());
        if (ce->callee && ce->callee->kind == NodeKind::IdentExpr) {
            auto* id = static_cast<IdentExpr*>(ce->callee.get());
            if (!id->name.empty() && id->name.front() == '#') {
                NodePtr expanded = tryExpandMacroCall(ce, id->name.substr(1));
                if (expanded) {
                    n = std::move(expanded);
                    if (expandDepth_ < 64) {
                        ++expandDepth_;
                        expandInNode(n);
                        --expandDepth_;
                    }
                }
            }
        }
    }
}

void Sema::walkChildren(Node* n, std::function<void(NodePtr&)> fn) {
    if (!n) return;
    auto each  = [&](NodeList& list) { for (auto& e : list) fn(e); };
    auto eachP = [&](std::vector<NodePtr>& list) { for (auto& e : list) fn(e); };
    switch (n->kind) {
    // ── 声明 ──
    case NodeKind::FunctionDecl: { auto* d = static_cast<FunctionDecl*>(n);
        fn(d->returnType);
        for (auto& p : d->params) { fn(p.type); fn(p.defaultValue); }
        each(d->body); break; }
    case NodeKind::InitDecl: { auto* d = static_cast<InitDecl*>(n);
        for (auto& p : d->params) { fn(p.type); fn(p.defaultValue); }
        each(d->body); break; }
    case NodeKind::DeinitDecl: { auto* d = static_cast<DeinitDecl*>(n);
        each(d->body); break; }
    case NodeKind::SubscriptDecl: { auto* d = static_cast<SubscriptDecl*>(n);
        for (auto& p : d->params) { fn(p.type); fn(p.defaultValue); }
        fn(d->elementType); each(d->getter); each(d->setter); break; }
    case NodeKind::StructDecl: case NodeKind::EnumDecl:
    case NodeKind::ClassDecl: case NodeKind::ActorDecl:
    case NodeKind::ProtocolDecl: case NodeKind::ExtensionDecl: {
        auto* d = static_cast<TypeDecl*>(n);
        each(d->inherited); fn(d->whereClause); each(d->members); break; }
    case NodeKind::VarDecl: { auto* d = static_cast<VarDecl*>(n);
        fn(d->type); fn(d->initializer); eachP(d->accessors); break; }
    case NodeKind::TypealiasDecl: { auto* d = static_cast<TypealiasDecl*>(n);
        fn(d->underlying); break; }
    case NodeKind::EnumCaseDecl: { auto* d = static_cast<EnumCaseDecl*>(n);
        eachP(d->associatedTypes); break; }
    case NodeKind::AssociatedTypeDecl: { auto* d = static_cast<AssociatedTypeDecl*>(n);
        each(d->inherited); fn(d->defaultType); break; }
    case NodeKind::MacroDecl: { auto* d = static_cast<MacroDecl*>(n);
        fn(d->returnType); each(d->body); break; }
    case NodeKind::AccessorDecl: { auto* d = static_cast<AccessorDecl*>(n);
        each(d->body); break; }
    // ── 语句 ──
    case NodeKind::BlockStmt: { auto* s = static_cast<BlockStmt*>(n);
        each(s->statements); break; }
    case NodeKind::ExprStmt: { auto* s = static_cast<ExprStmt*>(n); fn(s->expr); break; }
    case NodeKind::ReturnStmt: { auto* s = static_cast<ReturnStmt*>(n); fn(s->value); break; }
    case NodeKind::IfStmt: { auto* s = static_cast<IfStmt*>(n);
        fn(s->condition); each(s->thenBody); fn(s->elseBranch); break; }
    case NodeKind::IfExpr: { auto* s = static_cast<IfExpr*>(n);
        fn(s->condition); each(s->thenBody); fn(s->elseBranch); break; }
    case NodeKind::GuardStmt: { auto* s = static_cast<GuardStmt*>(n);
        fn(s->condition); each(s->elseBody); break; }
    case NodeKind::WhileStmt: { auto* s = static_cast<WhileStmt*>(n);
        fn(s->condition); each(s->body); break; }
    case NodeKind::LoopStmt: { auto* s = static_cast<LoopStmt*>(n);
        each(s->body); break; }
    case NodeKind::RepeatWhileStmt: { auto* s = static_cast<RepeatWhileStmt*>(n);
        each(s->body); fn(s->condition); break; }
    case NodeKind::ForInStmt: { auto* s = static_cast<ForInStmt*>(n);
        fn(s->pattern); fn(s->sequence); each(s->body); break; }
    case NodeKind::SwitchStmt: { auto* s = static_cast<SwitchStmt*>(n);
        fn(s->subject); eachP(s->cases); break; }
    case NodeKind::CaseClause: { auto* s = static_cast<CaseClause*>(n);
        fn(s->pattern); fn(s->whereExpr); each(s->body); break; }
    case NodeKind::DoStmt: { auto* s = static_cast<DoStmt*>(n);
        each(s->body); eachP(s->catches); break; }
    case NodeKind::CatchClause: { auto* s = static_cast<CatchClause*>(n);
        fn(s->pattern); fn(s->whereExpr); each(s->body); break; }
    case NodeKind::ThrowStmt: { auto* s = static_cast<ThrowStmt*>(n); fn(s->value); break; }
    case NodeKind::DeferStmt: { auto* s = static_cast<DeferStmt*>(n); fn(s->body); break; }
    case NodeKind::UnsafeStmt: { auto* s = static_cast<UnsafeStmt*>(n); each(s->body); break; }
    // ── 表达式 ──
    case NodeKind::BinaryExpr: { auto* e = static_cast<BinaryExpr*>(n);
        fn(e->lhs); fn(e->rhs); break; }
    case NodeKind::UnaryExpr: { auto* e = static_cast<UnaryExpr*>(n); fn(e->operand); break; }
    case NodeKind::CallExpr: { auto* e = static_cast<CallExpr*>(n);
        fn(e->callee); each(e->arguments); break; }
    case NodeKind::MemberExpr: { auto* e = static_cast<MemberExpr*>(n); fn(e->base); break; }
    case NodeKind::SubscriptExpr: { auto* e = static_cast<SubscriptExpr*>(n);
        fn(e->base); each(e->indices); break; }
    case NodeKind::OptionalChainExpr: { auto* e = static_cast<OptionalChainExpr*>(n); fn(e->expr); break; }
    case NodeKind::ForceUnwrapExpr: { auto* e = static_cast<ForceUnwrapExpr*>(n); fn(e->expr); break; }
    case NodeKind::TupleExpr: { auto* e = static_cast<TupleExpr*>(n); each(e->elements); break; }
    case NodeKind::ArrayLitExpr: { auto* e = static_cast<ArrayLitExpr*>(n); each(e->elements); break; }
    case NodeKind::DictLitExpr: { auto* e = static_cast<DictLitExpr*>(n);
        each(e->keys); each(e->values); break; }
    case NodeKind::SetLitExpr: { auto* e = static_cast<SetLitExpr*>(n); each(e->elements); break; }
    case NodeKind::ClosureExpr: { auto* e = static_cast<ClosureExpr*>(n);
        fn(e->returnType); each(e->body);
        for (auto& p : e->params) { fn(p.type); fn(p.defaultValue); } break; }
    case NodeKind::ParenExpr: { auto* e = static_cast<ParenExpr*>(n); fn(e->expr); break; }
    case NodeKind::AsExpr: { auto* e = static_cast<AsExpr*>(n); fn(e->expr); fn(e->type); break; }
    case NodeKind::IsExpr: { auto* e = static_cast<IsExpr*>(n); fn(e->expr); fn(e->type); break; }
    case NodeKind::AssignmentExpr: { auto* e = static_cast<AssignmentExpr*>(n); fn(e->lhs); fn(e->rhs); break; }
    case NodeKind::RangeExpr: { auto* e = static_cast<RangeExpr*>(n); fn(e->lower); fn(e->upper); break; }
    case NodeKind::GenericExpr: { auto* e = static_cast<GenericExpr*>(n); fn(e->base); each(e->args); break; }
    case NodeKind::MoveExpr: { auto* e = static_cast<MoveExpr*>(n); fn(e->operand); break; }
    case NodeKind::StrLitExpr: { auto* e = static_cast<StrLitExpr*>(n); each(e->expressions); break; }
    case NodeKind::TernaryExpr: { auto* e = static_cast<TernaryExpr*>(n);
        fn(e->condition); fn(e->thenValue); fn(e->elseValue); break; }
    case NodeKind::NamedType: { auto* t = static_cast<NamedType*>(n); eachP(t->genericArgs); break; }
    case NodeKind::OptionalType: { auto* t = static_cast<OptionalType*>(n); fn(t->wrapped); break; }
    case NodeKind::ArrayType: { auto* t = static_cast<ArrayType*>(n); fn(t->element); break; }
    case NodeKind::DictType: { auto* t = static_cast<DictType*>(n); fn(t->key); fn(t->value); break; }
    case NodeKind::TupleType: { auto* t = static_cast<TupleType*>(n); eachP(t->elements); break; }
    case NodeKind::FuncType: { auto* t = static_cast<FuncType*>(n); eachP(t->params); fn(t->ret); break; }
    case NodeKind::RefType: { auto* t = static_cast<RefType*>(n); fn(t->pointee); break; }
    case NodeKind::InoutType: { auto* t = static_cast<InoutType*>(n); fn(t->pointee); break; }
    case NodeKind::MetatypeType: { auto* t = static_cast<MetatypeType*>(n); fn(t->base); break; }
    case NodeKind::AsmExpr: { auto* a = static_cast<AsmExpr*>(n);
        for (auto& o : a->outputs) fn(o.expr);
        for (auto& i : a->inputs)  fn(i.expr);
        break; }
    default:
        break;
    }
}

// ─── 裸机属性校验（规范 §8.7）────────────────────────────────────────────────
// `@panic_handler` 的函数签名须为 `func panic(info: PanicInfo) -> Never`；
// `@global_allocator` 的类型须遵守 `GlobalAlloc` 协议。两者各至多一个，
// 校验通过后记录于 panicHandlerFn_ / globalAllocatorDecl_ 供 codegen 取用。
void Sema::validateBareMetalAttrs(const NodeList& decls) {
    auto hasAttribute = [](const Node* n, const char* attr) {
        for (const auto& a : n->attributes) if (a == attr) return true;
        return false;
    };
    for (auto& d : decls) {
        if (!d) continue;
        if (d->kind == NodeKind::FunctionDecl) {
            auto* fn = static_cast<FunctionDecl*>(d.get());
            if (!hasAttribute(fn, "panic_handler")) continue;
            bool ok = (fn->params.size() == 1);
            if (ok) {
                const Type* pt = fn->params[0].semaType;
                if (!pt || pt->kind != TypeKind::Named || pt->name != "PanicInfo") ok = false;
            }
            // 返回类型现解一次：标注节点的缓存 semaType 在部分路径下仍为
            // Unknown，直接按类型标注解析可稳定得到 `Never`。
            const Type* rt =
                fn->returnType ? resolveTypeRepr(fn->returnType.get(), nullptr) : nullptr;
            if (!rt || rt->kind != TypeKind::Never) ok = false;
            if (!ok) {
                hadError_ = true;
                diags_.reportError(
                    "'@panic_handler' requires signature 'func panic(info: PanicInfo) -> Never'",
                    rangeOf(d.get()));
            } else if (!panicHandlerFn_) {
                panicHandlerFn_ = fn;
            } else {
                hadError_ = true;
                diags_.reportError("duplicate '@panic_handler' function", rangeOf(d.get()));
            }
        } else if (d->kind == NodeKind::StructDecl || d->kind == NodeKind::ClassDecl ||
                   d->kind == NodeKind::EnumDecl) {
            auto* td = static_cast<TypeDecl*>(d.get());
            if (!hasAttribute(td, "global_allocator")) continue;
            bool conforms = td->semaType ? typeConformsTo(td->semaType, "GlobalAlloc") : false;
            if (!conforms) {
                hadError_ = true;
                diags_.reportError(
                    "'@global_allocator' type must conform to 'GlobalAlloc'", rangeOf(d.get()));
            } else if (!globalAllocatorDecl_) {
                globalAllocatorDecl_ = td;
            } else {
                hadError_ = true;
                diags_.reportError("duplicate '@global_allocator' type", rangeOf(d.get()));
            }
        }
    }
}

} // namespace suki
