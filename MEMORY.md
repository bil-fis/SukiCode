# SukiCode 项目开发记忆

## 项目概述
SukiCode 是一门融合 Swift、TypeScript 与 Objective-C 语法的现代系统编程语言，AOT 编译基于 LLVM 后端。

## 开发环境
- **操作系统**: Windows 11 Home China
- **IDE**: Visual Studio 2026 (MSVC 19.51)
- **构建系统**: CMake ≥ 3.20 + Ninja
- **LLVM**: 22.1，安装在 `D:\DevEnviroment\LLVM`，通过环境变量 `LLVM_DIR` 指向 `D:\DevEnviroment\LLVM\lib\cmake\llvm`
- **C++ 标准**: C++20
- **语言约定**: 所有对话和文档使用中文，代码注释使用中文英语双语，错误提示使用中文

## 里程碑
- **首次成功编译并运行**: `sukic -o hello.exe hello.suki && hello.exe` 输出 42
- **fibonacci(10) = 55**: 递归函数正确执行
- **@main 属性**: 自动重命名为入口函数
- **条件编译**: #if os(Windows) 正确工作
- **前向引用**: 函数可以在定义前调用
- **泛型函数**: `identity<T>(value: T) -> T` 正确工作（单态化）
- **复合赋值运算符**: `+=`, `-=` 等正确生成 IR
- **集成测试**: hello_world, fibonacci, generic, control_flow 全部通过
- **35 次提交**, 所有测试通过

## 已完成的核心管道
```
Lexer → Parser → AST → Sema → LLVM IR → Object File → Executable
```

## 整体完成度

| 维度 | 完成度 | 说明 |
|------|--------|------|
| 词法分析器 | **95%** | 核心功能完成，字符串插值标记化待完善 |
| 语法分析器 | **90%** | 所有声明/语句/表达式类型均有 AST 定义和解析逻辑 |
| 类型系统 | **65%** | 基本类型和复合类型框架存在，泛型基础已实现 |
| 语义分析 | **50%** | 类型推断、类型检查、作用域、控制流检查 |
| LLVM IR 代码生成 | **50%** | 函数/变量/控制流/表达式 IR 生成，ARC 框架 |
| 编译器驱动 | **40%** | 可产出可执行文件，目标代码和链接 |
| 运行时库 | **45%** | ARC、Pool、Collection、Channel、Atomic、Mutex |
| 标准库 | **15%** | Error/Result/Equatable/Hashable/String/Array/Dict/Optional/Range |
| **整体** | **~40%** | |

## 关键技术决策
- 使用手写递归下降解析器（非 ANTLR/TableGen），更好控制错误信息
- LLVM RTTI 设置：MSVC 用 `/GR-`，GCC/Clang 用 `-fno-rtti`
- MSVC 需要 `/utf-8` 编译选项支持中文注释
- 注释扫描不递归调用 `next()`，避免吞 token 的 bug
- `TypeDecl` 需要显式构造函数链 `TypeDecl(k) → Decl(k)`
- MSVC 的 `alloca` 是宏，变量名不能用 `alloca`，需用 `allocaInst`
- LLVM 22.1: `llvm/Support/Host.h` 改为 `llvm/TargetParser/Host.h`
- LLVM 22.1: `Module::setTargetTriple` 需要 `llvm::Triple` 而非 `std::string`
- LLVM 目标初始化使用 `LLVM_NATIVE_*` 宏（MSVC 不导出 `InitializeX86Target` 等函数）
- Windows 链接使用 `cl.exe` 而非 `link.exe`（Git Bash 的 `link` 是 Unix 版本）
- 字符串插值在解析器层面处理（临时词法分析器解析插值表达式）
- 前向引用通过预注册所有函数声明解决
- 泛型参数在预注册和函数体处理时都需要注册到符号表
- 泛型返回类型跳过类型兼容性检查（Any 类型）
- 泛型单态化：当泛型函数被调用时，根据参数类型生成特化版本
- 复合赋值运算符需要在 AST 中存储运算符种类

## 文件结构
```
src/compiler/
├── lexer/Token.h, Token.cpp, Lexer.h, Lexer.cpp
├── parser/Parser.h, Parser.cpp
├── ast/ASTNode.h, ASTNode.cpp, ASTPrinter.h, ASTPrinter.cpp
├── sema/Sema.h, Sema.cpp, Type.h, Type.cpp, SymbolTable.h, SymbolTable.cpp, TypeChecker.h, TypeChecker.cpp, Scope.h, Scope.cpp
├── codegen/IRGenerator.h, IRGenerator.cpp, TypeConverter.h, TypeConverter.cpp, ObjectEmitter.h, ObjectEmitter.cpp
├── diag/Diagnostic.h, Diagnostic.cpp
├── util/StringInterner.h, StringInterner.cpp
└── main.cpp
src/runtime/
├── arc/ARC.h, ARC.cpp, RefCount.h, RefCount.cpp, WeakRef.h, WeakRef.cpp
├── pool/MemoryPool.h, MemoryPool.cpp, Collection.h, Collection.cpp
└── concurrency/ThreadPool.h, ThreadPool.cpp, Coroutine.h, Coroutine.cpp, Channel.h, Atomic.h, Lock.h
src/stdlib/core/
├── Print.h, Print.cpp, Assert.h, Assert.cpp
├── Error.h, Equatable.h, String.h, String.cpp
├── Array.h, Dictionary.h, Optional.h, Range.h
```

## 未完成任务清单

### P3: 工具链/生态
- [ ] P3-1: SukiPM 包管理器
- [ ] P3-2: suki-lsp 语言服务器
- [ ] P3-3: suki-fmt 格式化器
- [ ] P3-4: 调试信息 DWARF
- [ ] P3-5: 增量编译
- [ ] P3-6: 跨平台编译目标三元组
- [ ] P3-7: 文档生成
- [ ] P3-8: 标准库模块 (System/Network/Crypto/Data/I18n/Test/CLI)

### 完善项
- [ ] 泛型实例化（单态化）、关联类型
- [ ] 完整协程状态机转换
- [ ] 跨 Actor 调用自动 await
- [ ] 引用计数在所有赋值/参数/返回时的完整插入
- [ ] 集合字面量 `{1, 2, 3}` 上下文相关解析
- [ ] 泛型构造调用 `Channel<Int>(capacity: 10)`

## 相关记忆
- [[suki-language-spec]] — SukiCode 语言规范文档 (SukiCode_Specification.md)
