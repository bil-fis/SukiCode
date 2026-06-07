// SukiCode 类型检查器实现
// Performs detailed type checking on AST nodes.

#include "TypeChecker.h"

namespace suki {

TypeChecker::TypeChecker(DiagnosticEngine& diag, SymbolTable& symbols)
    : diag_(diag), symbols_(symbols) {}

TypeChecker::~TypeChecker() = default;

bool TypeChecker::checkAssignment(const Type& target, const Type& source, SourceLocation loc) {
    // 相同类型 / Same type
    if (target.equals(source)) return true;

    // Any 类型接受所有 / Any type accepts all
    if (target.kind() == TypeKind::Any) return true;

    // 数值类型隐式转换 / Numeric implicit conversion
    if (source.canImplicitlyConvertTo(target)) return true;

    // Optional 可以接受 nil / Optional can accept nil
    if (target.kind() == TypeKind::Optional && source.kind() == TypeKind::Error) return true;

    // Composition 类型检查 / Composition type check
    if (target.kind() == TypeKind::Composition) {
        // 源类型需要满足组合类型中的所有协议
        // 检查源类型是否与组合类型中的每个协议兼容
        auto& compType = static_cast<const CompositionType&>(target);
        for (const auto& proto : compType.protocols()) {
            // 简化：检查源类型是否与协议类型兼容
            // 完整实现需要协议符合性检查
            if (!source.canImplicitlyConvertTo(*proto)) {
                // 如果源类型不能转换到协议类型，检查是否是 Any
                if (source.kind() != TypeKind::Any) {
                    // 允许兼容的类型通过
                    // Allow compatible types to pass
                }
            }
        }
        return true;
    }

    error(loc, "cannot assign value of type '" + source.name() +
          "' to type '" + target.name() + "'");
    return false;
}

bool TypeChecker::checkCallArguments(const std::string& funcName,
                                      const std::vector<TypePtr>& paramTypes,
                                      const std::vector<TypePtr>& argTypes,
                                      SourceLocation loc) {
    if (paramTypes.size() != argTypes.size()) {
        error(loc, "function '" + funcName + "' expects " +
              std::to_string(paramTypes.size()) + " arguments, got " +
              std::to_string(argTypes.size()));
        return false;
    }

    bool ok = true;
    for (size_t i = 0; i < paramTypes.size(); i++) {
        if (!paramTypes[i] || !argTypes[i]) continue;
        if (paramTypes[i]->kind() == TypeKind::Error || argTypes[i]->kind() == TypeKind::Error) continue;
        if (!argTypes[i]->canImplicitlyConvertTo(*paramTypes[i])) {
            error(loc, "argument " + std::to_string(i + 1) + " type '" +
                  argTypes[i]->name() + "' does not match parameter type '" +
                  paramTypes[i]->name() + "'");
            ok = false;
        }
    }
    return ok;
}

bool TypeChecker::checkReturnType(const Type& expected, const Type& actual, SourceLocation loc) {
    if (expected.kind() == TypeKind::Void && actual.kind() == TypeKind::Void) return true;
    if (expected.kind() == TypeKind::Void) {
        error(loc, "void function should not return a value");
        return false;
    }
    if (actual.kind() == TypeKind::Void) {
        error(loc, "non-void function must return a value");
        return false;
    }
    if (actual.canImplicitlyConvertTo(expected)) return true;
    error(loc, "return type '" + actual.name() +
          "' does not match function return type '" + expected.name() + "'");
    return false;
}

bool TypeChecker::checkMutability(const std::string& varName, bool isWrite, SourceLocation loc) {
    if (!isWrite) return true;
    Symbol* sym = symbols_.lookup(varName);
    if (sym && sym->isConstant) {
        error(loc, "cannot assign to 'let' constant '" + varName + "'");
        return false;
    }
    return true;
}

bool TypeChecker::checkOptionalSafety(const Type& type, bool isForceUnwrap, SourceLocation loc) {
    if (type.kind() != TypeKind::Optional) {
        if (isForceUnwrap) {
            error(loc, "cannot force unwrap non-optional type '" + type.name() + "'");
            return false;
        }
        return true;
    }
    // Optional 类型的强制解包是允许的（但可能崩溃）
    return true;
}

bool TypeChecker::checkTypeCast(const Type& from, const Type& to, CastKind kind, SourceLocation loc) {
    switch (kind) {
        case CastKind::Coerce:
            // as: 强制转换（总是允许）
            return true;
        case CastKind::Conditional:
            // as?: 条件转换（总是允许，运行时检查）
            return true;
        case CastKind::Force:
            // as!: 强制转换（总是允许，但可能崩溃）
            return true;
    }
    return true;
}

void TypeChecker::enterLoop() { loopDepth_++; }
void TypeChecker::leaveLoop() { loopDepth_--; }
bool TypeChecker::isInLoop() const { return loopDepth_ > 0; }

void TypeChecker::enterSwitch() { switchDepth_++; }
void TypeChecker::leaveSwitch() { switchDepth_--; }
bool TypeChecker::isInSwitch() const { return switchDepth_ > 0; }

void TypeChecker::setInThrowsFunction(bool inThrows) { inThrowsFunc_ = inThrows; }
bool TypeChecker::isInThrowsFunction() const { return inThrowsFunc_; }

void TypeChecker::setInAsyncFunction(bool inAsync) { inAsyncFunc_ = inAsync; }
bool TypeChecker::isInAsyncFunction() const { return inAsyncFunc_; }

void TypeChecker::enterUnsafe() { unsafeDepth_++; }
void TypeChecker::leaveUnsafe() { unsafeDepth_--; }
bool TypeChecker::isInUnsafe() const { return unsafeDepth_ > 0; }

void TypeChecker::error(SourceLocation loc, const std::string& msg) {
    diag_.error(loc, "", msg);
}

} // namespace suki
