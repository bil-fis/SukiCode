# SukiCode 编译器未实现功能 — 详细实现计划

> 对照 `SukiCode_Specification.md`（v1.0.0）整理。状态来自对 `src/compiler`、`src/runtime`、`src/stdlib` 的审计。
> 已完整实现（类型检查 + 代码生成/运行时）的功能不列入本计划，包括：错误处理 throws/try/do-catch、ARC 自动 retain/release + weak、willSet/didSet、final/override、Set、`fallthrough`/`repeat-while`/`guard`、区间 `..<`/`...`、命名元组/typealias、协议/extension 默认实现、deinit、下标 get/set、inout、默认参数+变长、@escaping、[weak self]。
>
> 落地文件约定：
> - 解析：`src/compiler/parser/Parser.cpp`、`src/compiler/lexer/Lexer.cpp`、`src/compiler/ast/AST.h`
> - 语义：`src/compiler/sema/Sema.cpp`、`src/compiler/sema/Sema.h`
> - 代码生成：`src/compiler/codegen/IRGeneratorImpl.cpp`、`src/compiler/codegen/IRGenerator.h`、`src/compiler/codegen/TypeLayout.*`
> - 运行时：`src/runtime/runtime.c`、`src/runtime/runtime.h`
> - 标准库：`src/stdlib/**`（目前为占位，需恢复）
> - 驱动：`src/compiler/main.cpp`

---

## 阶段 0：编译器后端与 CLI 基座（其它特性的前提）

### P0.1 自包含 LLVM 后端 / LLD 链接（规范 §14.1.3）
- 现状：`main.cpp` 的 `compileOne()`/`commandBuild()`/`commandRun()` 仍调用外部 `clang`（`SUKI_CLANG_DRIVER "clang"`）把 `.ll` 降为目标文件并链接，违反「不依赖外部系统编译器、用 LLD」的规范。
- 待实现：
  - 在驱动中引入 LLVM C++ API：用 `llvm::orc` 或静态 `llvm::Target` + `AsmPrinter` 直接产出 `.o`（替换 `std::system("clang -c ...")`）。
  - 链接改用 LLD：`lld::elf::link` / `lld::coff::link` / `lld::wasm::link`（按目标格式分发）。
  - 新增选项 `-fuse-ld=lld`（默认开）、`--integrated-as`（默认开，调试用 `--no-integrated-as`）。
- 落地文件：`src/compiler/main.cpp`、`src/compiler/codegen/`（新增 `LLVMEmit.*` 封装）、CMake 链接 `LLVMCore/CodeGen/MC/Support` 与 `lld` 库。

### P0.2 缺失的 CLI 选项（§14.1.1/14.1.2/14.1.3）
- 现状：`main.cpp` 仅支持 `lex/parse/check/emit-ir/build/run` + `--target`/`--dump-ast`/`--emit-ir`；无 `--list-targets`、`--incremental`、`-S`、`--emit-llvm`、`--sysroot`、`-O*`。
- 待实现：
  - `--list-targets`：遍历 `llvm::TargetRegistry` 打印所有可用 target triple。
  - `-S`：输出汇编文本（用 `Target` + `AsmPrinter`）。
  - `--emit-llvm`：直接输出 `.ll` / `.bc`（复用现有 `emitIR`，改名暴露为独立命令）。
  - `-c`：支持多文件分别编译为 `.o`（当前 `commandBuild` 限制 `files.size()!=1` → 改为遍历并按模块聚合）。
  - `--sysroot=<path>`：透传给 LLD 的 `-L`/`-dynamic-linker` 解析。
  - 优化级别 `-O0/-O1/-O2/-Os/-O3`：传给 LLVM `PassBuilder` / `OptLevel`，并映射 `debug`/`release`/`size` 模式（§10.4）。
  - `--incremental`：在 `.build/cache` 按「源文件内容 hash + 依赖图」缓存 IR/object，仅重编变更文件及其依赖（需依赖扫描，见 P1.4 模块边界）。
- 落地文件：`src/compiler/main.cpp`。

### P0.3 DWARF 调试信息（§14.3）
- 现状：codegen 不生成调试信息。
- 待实现：在 `IRGeneratorImpl.cpp` 用 `llvm::DIBuilder` 为每个函数/变量/类型生成 `!DISubprogram`/`!DILocalVariable`/`!DILocation`；泛型实例化类型名也纳入 DWARF。
- 落地文件：`src/compiler/codegen/IRGeneratorImpl.cpp`（新增 `DebugInfo` 辅助）。

---

## 阶段 1：类型系统与声明补全

### P1.1 访问控制 public/internal/fileprivate/private（§10.1）
- 现状：Sema 中 0 个 `AccessLevel` 强制逻辑。
- 待实现：
  - `AST.h` 的 Decl 基类增加 `accessLevel` 字段；`Parser.cpp` 解析四个关键字修饰符。
  - `Sema.cpp` 在 `lookupMember` / 跨文件 import 解析时按访问级别过滤：`internal` 仅同模块可见，`fileprivate` 仅当前文件，`private` 仅当前作用域（`ScopedTable` 已支持层级，复用即可）。
  - 越权使用（如模块外访问 `internal` 符号）通过 `DiagnosticEngine` 报错。
- 落地文件：`AST.h`、`Parser.cpp`、`Sema.cpp`。

### P1.2 条件编译 #if（§10.3）
- 现状：无 `IfConfig` 节点、无预处理器（compiler 内 0 匹配）。
- 待实现：
  - `Lexer.cpp` 识别 `#if`/`#elseif`/`#else`/`#endif`/`#if canImport` 等预处理行。
  - 新增配置传递：`-D NAME[=VALUE]`（main.cpp 解析 → 全局宏表）。
  - 预解析阶段求值 `os(Linux)`/`os(macOS)`/`os(Windows)`/`arch(x86_64)`/`suki(>=1.0)`/自定义 `-D`，裁剪 AST（保留命中分支，丢弃其余）。
  - `os/arch` 取自 `TargetInfo`（`resolveTarget` 已有）。
- 落地文件：`Lexer.cpp`、新增 `src/compiler/sema/Preprocessor.*`、`main.cpp`、`TargetInfo.cpp`。

### P1.3 嵌套类型（§4.6）
- 现状：`Parser.cpp` 设了 `enclosingType` 标志，但 Sema/Codegen 不使用，`Outer.Inner` 的 name lookup 失败。
- 待实现：
  - `Sema.cpp` 的符号表记录嵌套声明的外层类型作用域；`resolveType` / `lookupMember` 支持 `Outer.Inner` 路径解析。
  - 嵌套类型可访问外层 static 成员（复用 `ScopedTable` 层级向上查找）。
  - `Codegen`：类型 mangling 用 `Outer.Inner` 前缀；内层方法 `self` 类型指向内层类型。
- 落地文件：`Sema.cpp`、`IRGeneratorImpl.cpp`。

### P1.4 多文件模块 / import 语义（§1.1/10.2）
- 现状：`build` 仅单文件；`ModuleDecl`/`ImportDecl` 仅解析，无跨文件符号解析。
- 待实现：
  - 驱动支持一次接收同模块多文件，合并 AST 后再跑 Sema（为 P1.1 访问控制、P0.2 incremental 提供模块边界）。
  - `ImportDecl` 解析为「依赖模块符号表」引用；跨模块查 `public` 符号。
- 落地文件：`main.cpp`、`Sema.cpp`。

### P1.5 @enum(C) C 兼容布局（§2.3）
- 现状：compiler 内无 `@enum`/`enum(C)`/`CEnum` 处理。
- 待实现：
  - `Parser.cpp` 解析枚举上的 `@enum(C)` 属性；`Sema.cpp` 标记该枚举为 C 布局（禁止关联值，仅允许原始值或空载荷）。
  - `TypeLayout` 增加 C-enum 分支：无 tag，仅 union（成员按最大 size 重叠，offset 0）。
  - `Codegen` 按 C 布局生成 LLVM 结构体。
- 落地文件：`Parser.cpp`、`Sema.cpp`、`TypeLayout.cpp`。

### P1.6 required / convenience init（§4.4）
- 现状：仅 `Parser.cpp` 解析，Sema 无强制（`final`/`override` 已实现）。
- 待实现：
  - `Sema.cpp` 增加 `initKind`（designated/required/convenience）。
  - 遍历继承链：标记 `required` 的 init，其所有子类必须实现对应 init（缺则报错）。
  - `convenience` init 首个语句必须是 `self.init(...)`，且链最终落到 designated init（否则报错）。
- 落地文件：`Sema.cpp`、`AST.h`。

### P1.7 Any / AnyObject 装箱（§2.8）
- 现状：仅当类型名识别，无真实存在容器/动态派发。
- 待实现：
  - `Sema.cpp`：`Any` 值类型 → 生成存在容器（type erasure：{payload 字节, typeId, vtable 指针}）；`AnyObject` 引用类型包装。
  - `as?`/`as!` 拆箱：在 codegen 生成 typeId 检查 + 位转换。
  - `Codegen`：新增 `genExistentialBox`/`genExistentialUnbox`。
- 落地文件：`Sema.cpp`、`IRGeneratorImpl.cpp`。

### P1.8 String 视图（§1.5）
- 现状：插值已实现；`.unicodeScalars`/`.characters`/`.utf8`/`.utf16` 视图 0 匹配。
- 待实现：在 `src/stdlib` 恢复 `String` 内部 COW 存储，并实现四个 `Collection` 视图（`unicodeScalars`/`characters`/`utf8`/`utf16`）。
- 落地文件：`src/stdlib/core/String.suki`（需先恢复 stdlib 骨架）。

---

## 阶段 2：泛型增强

### P2.1 泛型 where 子句 + 协议约束强制（§5.3）
- 现状：`whereClause` 在 Sema 0 匹配；仅 switch 的 `case … where` 被查；`T: Equatable` 未强制。
- 待实现：
  - `Parser.cpp` 解析 `func f<T>(...) where T: Equatable & Hashable` / `T == Int` / `where T: X & Y`；新增 `WhereClause` AST 结构。
  - `Sema.cpp`：在单态化/调用点对 `T` 实参执行 conformance 检查（复用 `conformsTo`），不满足报 `error: type X does not conform to Y`。
  - `Codegen`：单态化机制已存在，无需额外（按具体类型实例化即可）。
- 落地文件：`AST.h`、`Parser.cpp`、`Sema.cpp`。

### P2.2 associatedtype 约束绑定强制（§5.4）
- 现状：`associatedtype` 被收集（`Sema.cpp:529/536`），但「带 associatedtype 的协议遵守绑定」大概率未强制。
- 待实现：
  - 类型遵守含 `associatedtype` 的协议时，必须提供关联类型绑定（通过类型别名或调用点推断）。
  - `checkConformances`（`Sema.cpp:342-368`）增加：缺失关联类型绑定 → 报错。
- 落地文件：`Sema.cpp`。

### P2.3 不透明返回类型 `some`（§5.5）
- 现状：codegen 的 `.some` 只是 Optional，非不透明返回。
- 待实现：
  - `Sema.cpp`：返回类型 `some P` → 在调用点用函数体推导出的具体底层类型替换（隐藏具体类型）。
  - 校验函数体所有 `return` 为同一具体类型。
  - `Codegen`：解析为具体类型后正常生成。
- 落地文件：`Sema.cpp`、`IRGeneratorImpl.cpp`。

---

## 阶段 3：宏系统（§5.6）

### P3.1 重建 MacroExpander（已被删除）
- 现状：`src/compiler/macro/MacroExpander.h` 在本次重构中删除，仅剩 `MacroDecl` AST 节点，无展开逻辑。
- 待实现：
  - 重建 `src/compiler/macro/MacroExpander.h` + `.cpp`。
  - `Parser.cpp`：解析 `@macro func ...`、`@freestanding`/`@attached` 属性；`@macro` 在 Sema 前插入 MacroPass（AST→AST 变换）。
  - **沙盒**：宏体在受限求值环境执行，禁止文件 I/O/网络/进程/系统调用（仅操作 Syntax 节点与字符串）。
  - **卫生性**：宏生成符号自动加唯一作用域前缀；`#unique("base")` 生成全局唯一名。
  - `Codegen`：`MacroDecl` 展开后变成普通 decl 序列参与后续流程。
- 落地文件：`src/compiler/macro/MacroExpander.*`、`Parser.cpp`、`Sema.cpp`（macro pass 钩子）。

---

## 阶段 4：内存与系统编程

### P4.1 unsafe 块强制（§8.6）
- 现状：`UnsafeStmt` 当普通块处理，裸指针随处可解析，无「unsafe 外禁止」检查。
- 待实现：
  - `Sema.cpp` 引入 `unsafeContext` 状态标志；进入/退出 `UnsafeStmt` 切换。
  - `UnsafePointer`/`UnsafeMutablePointer`/`Unmanaged` 类型使用、`asm` 语句、`@_unsafe` 函数调用、`takeRetainedValue/takeUnretainedValue` 仅允许在 unsafe context 内；块外使用 → 诊断报错。
- 落地文件：`Sema.cpp`、`AST.h`（UnsafeStmt 已存在）。

### P4.2 Owned<T> 移动语义 + 禁止 Owned<class>（§6.4）
- 现状：`MoveExpr` 仅做「标记 moved + use-after-move 诊断」（`Sema.cpp:1902-1907`）；codegen 无所有权转移；`Owned<class>` 不报错。
- 待实现：
  - `Sema.cpp`：`Owned<class>` 实例化 → 报错（与 ARC 多引用模型冲突）；`move x` 标记 x 为 moved（已有）。
  - `Codegen`：`Owned<T>` 值类型按移动传参/返回，禁止隐式拷贝（不 retain）；接收 `Owned` 参数后释放所有权。新增 `genMoveValue` ABI。
- 落地文件：`Sema.cpp`、`IRGeneratorImpl.cpp`。

### P4.3 unowned（§6.1）
- 现状：仅识别（`Sema.cpp:818`），codegen 0 匹配，当 strong 处理。
- 待实现：
  - `runtime.c` 新增 `suki_unowned_*`：非可选引用，访问时断言对象存活，不自动置 nil。
  - `Codegen`：`[unowned self]` 捕获与 `unowned var` 生成 unowned 引用读取。
- 落地文件：`runtime.c`、`IRGeneratorImpl.cpp`。

### P4.4 内联汇编 asm（§8.2）
- 现状：无 `AsmStmt` 节点，无解析/生成。
- 待实现：
  - `AST.h` 新增 `AsmStmt`（含 template 串、输出/输入操作数、clobber 列表）。
  - `Parser.cpp`：解析 `unsafe { asm("..." : "=r"(out) : "r"(in) : "cc", "memory"); }`。
  - `Codegen`：生成 LLVM `asm`/`callbr` 内联汇编 IR，约束映射 `r`/`m`/`i`/`=r`/`+r` 与 `"cc"`/`"memory"`。
- 落地文件：`AST.h`、`Parser.cpp`、`IRGeneratorImpl.cpp`。

### P4.5 MemoryLayout<T>（§8.1）
- 现状：0 匹配（stdlib 类型）。
- 待实现：`Sema.cpp`/`Codegen`：内建 `MemoryLayout<T>.size`/`stride`/`alignment` → 在 codegen 阶段用 `TypeLayout` 计算为常量内联（无需运行时）。
- 落地文件：`IRGeneratorImpl.cpp`、`TypeLayout.cpp`。

### P4.6 MMapRegion / DynamicLibrary / sys 模块（§8.3/8.4/8.5）
- 现状：runtime/stdlib 中 0 匹配（已删除）。
- 待实现：在恢复后的 `src/stdlib` 中实现：
  - `MMapRegion`：`mmap`/`munmap` 封装。
  - `DynamicLibrary`：`dlopen`/`dlsym`/`dlclose` 包装 + `lookup("sym")`。
  - `sys` 模块：`fork`/`exec`/`waitpid`/`open`/`read`/`write`/`ioctl`/`socket`/`bind`/`listen`/`accept`/`signal`/`sigaction`/`clock_gettime`/`nanosleep` 薄封装，失败抛 `SystemError`（code=errno, message=strerror）。
- 落地文件：`src/stdlib/system/**`、`src/runtime/**`（薄封装）。

### P4.7 裸机支持（§8.7）
- 现状：无 `--target bare-metal`、`_start`、`#panic_handler`、`#global_allocator` 处理。
- 待实现：
  - `main.cpp`：`--target=*-none-*` / `--target bare-metal` 模式禁用 stdlib、仅 `core` 模块。
  - `Parser.cpp`：解析 `@no_mangle`、`@_cdecl`、`extern "C" fn _start()`、`@panic_handler`、`#[global_allocator]`。
  - `Codegen`：生成 `_start` 入口、panic_handler 符号；不调用 ARC/runtime 初始化。
- 落地文件：`main.cpp`、`Parser.cpp`、`IRGeneratorImpl.cpp`。

---

## 阶段 5：并发与异步

### P5.1 async / await 状态机（§7.2）
- 现状：`isAsync/isAwait/suspend/resume/StateMachine` 在 Sema 0 匹配；codegen 仅 `if (u->isTry || u->isAwait) return v;` 透传。
- 待实现：
  - `Sema.cpp`：标记 async 函数、识别 `await` 点。
  - `Codegen`：将 async 函数体转为状态机——用 LLVM coroutine（`@llvm.coro.id`/`resume`/`suspend`/`end` intrinsics）split 为 resume/suspend；每个 `await` 生成挂起点；async 返回类型包装为 `Future<T>`/`TaskHandle`。
  - `runtime.c` 增加协程调度/挂起恢复运行时。
- 落地文件：`Sema.cpp`、`IRGeneratorImpl.cpp`、`runtime.c`。

### P5.2 actor 隔离与串行执行器（§7.4）
- 现状：actor 仅当普通 class 处理，无隔离/执行器。
- 待实现：
  - `Sema.cpp`：actor 方法默认 async；`nonisolated` 标记豁免隔离。
  - `Codegen`：actor 实例挂一个串行执行器（消息队列/单线程）；方法调用转 `await` 入队串行执行；可重入：await 后重查条件（编码约定，编译器生成重入安全桩）。
- 落地文件：`Sema.cpp`、`IRGeneratorImpl.cpp`、`runtime.c`（actor 执行器）。

### P5.3 Task / TaskGroup / Future（§7.1/7.3）
- 现状：仅词法关键字（Token.h），无 Sema/Codegen。
- 待实现：`runtime` + `stdlib`：
  - 全局协作线程池（工作窃取调度）。
  - `Future<T>.await(timeout:)` 超时抛 `TimeoutError`；响应父任务取消抛 `CancellationError`。
  - `Task`（可取消、`isCancelled`、`withTaskCancellationHandler`、`Task.sleep`）、`TaskGroup`（`withTaskGroup` 并行子任务、子任务继承取消）。
- 落地文件：`src/stdlib/concurrency/**`、`runtime.c`（线程池）。

### P5.4 Channel + select 语句（§7.5）
- 现状：`Parser.cpp` 把 select 解析为无 subject 的 `SwitchStmt`；codegen 中 channel 收发 0 匹配。
- 待实现：
  - `AST.h` 新增 `SelectStmt`（区分 send 分支 `value <- ch` / recv 分支 `let v <- ch` / `default`）。
  - `Parser.cpp`：正确解析 `select { case … <- ch: … case ch <- v: … default: … }`。
  - `Sema.cpp`：校验 select 在 async 上下文；分支模式匹配。
  - `Codegen`：多路等待 + 公平随机选一个就绪分支；非阻塞（`default`）路径。
  - `stdlib`：`Channel<T>` 有界（容量 + 背压 `.block/.dropNewest/.dropOldest/.throw`）/无界模式。
- 落地文件：`AST.h`、`Parser.cpp`、`Sema.cpp`、`IRGeneratorImpl.cpp`、`src/stdlib/concurrency/**`。

### P5.5 @MainActor / @executor 自定义执行器（§7.2）
- 现状：0 匹配。
- 待实现：`Sema.cpp`：函数/类型上的 `@MainActor`/`@executor(X)` 属性 → 调用点强制在对应执行器运行（复用 P5.2 actor 执行器机制）；跨执行器调用插入 hop（状态保存/恢复）。
- 落地文件：`Sema.cpp`、`IRGeneratorImpl.cpp`。

### P5.6 原子与锁（§7.6）
- 现状：stdlib 已删，0 匹配。
- 待实现：`stdlib` 实现 `Atomic<T>`（整数/指针特化）、`Mutex`/`RWLock`/`Semaphore`/`Condition`/`DispatchQueue`（基于 pthread + LLVM atomic intrinsics）。
- 落地文件：`src/stdlib/concurrency/**`。

---

## 阶段 6：错误处理补全与互操作

### P6.1 Result<T,E>（§9.4）
- 现状：属 stdlib（已删），编译器无此类型。
- 待实现：`stdlib` 实现 `Result<T, E: Error>` 枚举（`.success`/`.failure`）+ 常用方法。
- 落地文件：`src/stdlib/core/Result.suki`（恢复）。

### P6.2 Error / CustomStringConvertible / localizedDescription（§9.3）
- 现状：`Error` 协议与默认 `localizedDescription` 属 stdlib（已删）；编译器只生成 `SukiError` ABI。
- 待实现：`stdlib` 实现 `Error` 空协议 + 默认 `localizedDescription`（`String(describing: self)`）；`CustomStringConvertible` 协议。
- 落地文件：`src/stdlib/**`。

### P6.3 extern "stdcall" 与 @_cdecl（§13.1/13.3）
- 现状：`foreign` 外部链接声明已实现（`declareExternal`），但 `stdcall` 调用约定与 `@_cdecl` 自定义符号名 0 匹配。
- 待实现：
  - `Sema.cpp`：`extern` 声明增加调用约定字段（C/stdcall → LLVM `callcc`/函数 attribute）；`extern "stdcall" func ...` 解析。
  - `@_cdecl("sym")`：仅允许自由函数，设置导出符号名（当前 `declareExternal` 用默认名，需支持自定义 C 符号名）。
- 落地文件：`Sema.cpp`、`Parser.cpp`、`IRGeneratorImpl.cpp`。

### P6.4 #selector（§13.2.4）
- 现状：0 匹配。
- 待实现：`Parser.cpp`/`Sema.cpp`：`#selector(method)` → 展开为 `sel_registerName("method")` 调用（依赖 P6.5 ObjC 运行时预声明）。
- 落地文件：`Parser.cpp`、`Sema.cpp`。

### P6.5 Objective-C 互操作（§13.2）
- 现状：无 ObjC 运行时预声明/消息发送。
- 待实现：
  - `Sema.cpp`：自动注入 `objc_msgSend`/`sel_registerName`/`objc_getClass`/`objc_retain`/`objc_release` 等 extern 预声明。
  - `Codegen`：生成 `objc_msgSend` 调用 IR；ObjC 对象复用现有 ARC（weak/unowned）管理。
- 落地文件：`Sema.cpp`、`IRGeneratorImpl.cpp`。

---

## 阶段 7：独立工具（§14.2/14.4/14.6/11）

### P7.1 suki-lsp（§14.2）
- 现状：`tools/` 下应有占位。
- 待实现：补全 LSP：补全、跳转定义、重构、错误提示（复用 Sema 诊断）。
- 落地文件：`tools/suki-lsp/**`。

### P7.2 suki-fmt（§14.4）
- 现状：独立工具。
- 待实现：按 §1.9 风格规则格式化（4 空格缩进、大括号位置、运算符空格、命名约定），支持 `.suki-fmt.json` 配置。
- 落地文件：`tools/suki-fmt/**`。

### P7.3 sukipm + docs（§11/14.6）
- 现状：包管理器未实现（`sukic build` 不调用 SukiPM）。
- 待实现：`<Package>.sukiproj` 清单解析、依赖解析（PubGrub）、`sukipm init/build/test/publish/add/remove/update/docs`；`sukipm docs` 从 `///` 注释提取生成 HTML/Markdown。
- 落地文件：`tools/sukipm/**`。

---

## 实施顺序建议（依赖关系）

1. **P0（基座）** → 必须先做，否则后续特性无法独立验证/链接。
2. **P1（类型系统）** → P1.4（多文件模块）是 P1.1（访问控制）、P0.2（incremental）的前提。
3. **P2（泛型增强）** → 独立，可在 P1 之后。
4. **P3（宏）** → 独立，建议在 Sema 管线稳定后做。
5. **P4（内存/系统）** → P4.1 unsafe 强制应早于 P4.4 asm（asm 需在 unsafe 内）；P4.6/P4.7 依赖 stdlib 骨架恢复。
6. **P5（并发）** → P5.1 async 状态机是 P5.2~P5.5 的共同底座；P5.3/P5.4 依赖 stdlib 恢复。
7. **P6（错误/互操作）** → P6.1/P6.2 依赖 stdlib 恢复；P6.4/P6.5 依赖彼此。
8. **P7（工具）** → 可并行于以上，但 LSP/fmt 依赖 Sema 稳定。

> 注：所有依赖 `src/stdlib` 恢复的特性（P1.8、P4.6、P5.3、P5.4、P5.6、P6.1、P6.2）需先重建 stdlib 骨架（当前 `src/stdlib/CMakeLists.txt` 仅占位 "Placeholder for now."）。
