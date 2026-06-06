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

## 测试文件约定
- 所有测试文件（test_*.suki）放到 `moduleTest/` 目录
- 测试分类：`moduleTest/lexer/`、`moduleTest/parser/`、`moduleTest/sema/`、`moduleTest/codegen/`、`moduleTest/runtime/`、`moduleTest/integration/`
- 测试构建产物放到 `moduleTest/build/`（已 git 排除）
- 不要在 `/tmp` 或项目根目录放测试文件

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

## 项目状态 (2026-06-06)
- **编译器前端**: 词法分析、语法分析、语义分析完整实现
- **代码生成**: LLVM IR 生成支持所有主要特性
  - Optional 使用 {value, hasValue} 标记结构体
  - Enum 根据 associated values 计算 payload 大小
  - super 实现父类类型查找
  - Self 返回当前处理的类型
  - 闭包变量捕获和返回类型
  - for-in 数组迭代、可选链、强制解包、类型检查
- **语义检查**: 协议符合性、switch 穷举性、修饰符传播、let/var 可变性
- **标准库**: Array、Dictionary、Set、String、Optional、Result、Range、Stack、Queue、LinkedList
- **系统库**: MemoryLayout、DynamicLibrary、SystemInfo、Process、File、Path、Date/Timer、sys 模块
- **并发库**: ThreadPool、DispatchQueue、Channel、Atomic、Mutex/RWLock/Semaphore
- **加密库**: SHA256、MD5、HMAC-SHA256、AES-128 ECB、ChaCha20、RSA、Ed25519
- **数据编码**: Base64、Hex、GZip、Zlib、XML、CSV、MessagePack
- **网络库**: URL、JSON、Socket（TCP/UDP）、URLSession HTTP 客户端
- **国际化**: LocalizedString、NumberFormatter、CurrencyFormatter、DateFormatter
- **测试**: 20/20 测试全部通过
- **TODO**: 0 个待办事项
- **死代码**: 已清理 Scope.h/cpp、TypeConverter.h/cpp、StringInterner.h/cpp

## 相关记忆
- [[suki-language-spec]] — SukiCode 语言规范文档 (SukiCode_Specification.md)
