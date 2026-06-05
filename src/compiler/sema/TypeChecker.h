#pragma once
// SukiCode 类型检查器
// Performs detailed type checking on AST nodes.

#include "compiler/ast/ASTNode.h"
#include "compiler/diag/Diagnostic.h"
#include "Type.h"
#include "SymbolTable.h"
#include <string>

namespace suki {

class TypeChecker {
public:
    TypeChecker(DiagnosticEngine& diag, SymbolTable& symbols);
    ~TypeChecker();

    // 检查赋值类型兼容性 / Check assignment type compatibility
    bool checkAssignment(const Type& target, const Type& source, SourceLocation loc);

    // 检查函数参数类型 / Check function argument types
    bool checkCallArguments(const std::string& funcName,
                            const std::vector<TypePtr>& paramTypes,
                            const std::vector<TypePtr>& argTypes,
                            SourceLocation loc);

    // 检查返回类型 / Check return type
    bool checkReturnType(const Type& expected, const Type& actual, SourceLocation loc);

    // 检查 let 常量不可变性 / Check let immutability
    bool checkMutability(const std::string& varName, bool isWrite, SourceLocation loc);

    // 检查可选类型安全 / Check optional type safety
    bool checkOptionalSafety(const Type& type, bool isForceUnwrap, SourceLocation loc);

    // 检查类型转换合法性 / Check type cast validity
    bool checkTypeCast(const Type& from, const Type& to, CastKind kind, SourceLocation loc);

    // 检查循环上下文 / Check loop context
    void enterLoop();
    void leaveLoop();
    bool isInLoop() const;

    // 检查 switch 上下文 / Check switch context
    void enterSwitch();
    void leaveSwitch();
    bool isInSwitch() const;

    // 检查 throws 上下文 / Check throws context
    void setInThrowsFunction(bool inThrows);
    bool isInThrowsFunction() const;

    // 检查 async 上下文 / Check async context
    void setInAsyncFunction(bool inAsync);
    bool isInAsyncFunction() const;

    // 检查 unsafe 上下文 / Check unsafe context
    void enterUnsafe();
    void leaveUnsafe();
    bool isInUnsafe() const;

private:
    void error(SourceLocation loc, const std::string& msg);

    DiagnosticEngine& diag_;
    SymbolTable& symbols_;
    int loopDepth_ = 0;
    int switchDepth_ = 0;
    int unsafeDepth_ = 0;
    bool inThrowsFunc_ = false;
    bool inAsyncFunc_ = false;
};

} // namespace suki
