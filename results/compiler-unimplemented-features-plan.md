# SukiCode 编译器未实现功能 — 实现计划（审计修正版）

> 对照 `SukiCode_Specification.md`（**第 11 次修订**）整理。状态来自对 `src/compiler`、`src/runtime`、`src/stdlib`、`tools` 的**全量代码审计**（使用代码探索 agent 通读真实源码 + `moduleTest` 测试覆盖反推），而非 v1.0.0 旧计划的臆测。
>
> **审计关键结论**：v1.0.0 旧计划严重低估了前端进度。以下特性经核实**已实现**，已移出本计划：
> `#if` 条件编译、`public/internal/fileprivate/private` 访问控制、嵌套类型 `Outer.Inner`、

`required`/`convenience` init 链、`where T: Equatable` 约束、`associatedtype` 绑定强制、`some` 不透明返回（协议约束暂略）、`unsafe {}` 上下文强制、`Owned<class>` 禁止实例化、内联汇编 `asm`、`MemoryLayout<T>`、`@_cdecl` 自定义符号名；以及旧计划已标注的 throws/ARC+weak/willSet/didSet/final/override/Set/fallthrough/repeat-while/guard/区间/命名元组/typealias/协议扩展默认实现/deinit/下标/inout/默认参数+变长/@escaping/[weak self]。
>
> 本计划**只覆盖仍未实现**的部分，按依赖关系分阶段。每条给出：现状（含代码证据）、待实现、落地文件。

## 关键阻塞（必须先消解）

- **BLK-A：`src/stdlib/` 为空**（仅 `CMakeLists.txt` 占位 `"Placeholder for now."`）。C runtime（`src/runtime/runtime.c`）是**真实的**（ARC/weak/array/dict/string/mutex/atomic 齐全），阻塞点在 `.suki` 标准库骨架。被整体阻塞：String 视图、Result、Error/CustomStringConvertible、Atomic/Mutex 上层类型、MMap/DynamicLibrary/sys、Task/Future/TaskGroup、Channel、actor 执行器、COW。
- **BLK-B：后端仍依赖外部 clang**。`main.cpp:791-877` 用 `std::system("clang -c ...")` 把 `.ll` 降为目标文件并链接（同时链接 `runtime.c`），无自包含 LLVM（`AsmPrinter`/`Target`/`MC`）、无 LLD。这同时阻塞 `-S`/`--emit-llvm`/`-c` 多文件/`--sysroot`/`-O*`/`--incremental` 与 DWARF 的落地。

---

## 阶段 0：关键阻塞消解

### BLK-A 重建 `src/stdlib` 骨架 — ✅ 已完成（2026-10-04）
- **现状（完成）**：已新增 `src/stdlib/{core,concurrency,system,objc}/*.suki` 四个模块源文件与 CMake 清单。`main.cpp` 增加 `loadImportedStdlib`（**import 驱动**的最小模块加载器：仅当用户写 `import X` 时，把 `SUKI_STDLIB_DIR/<X>/<X>.suki` 解析并预编译进同一翻译单元，既不破坏现有测试套件、又实现了「能 import 并链接」）；`CMakeLists` 注入 `SUKI_STDLIB_DIR` / `SUKI_STDLIB_FILES`（glob 得到的标准库文件清单）。
- **关键修复**：`IRGeneratorImpl.cpp` 的 `generateBodyAs` 循环增加 `if (pb.first->isForeign) continue;`——`foreign` 函数此前会被错误地发射空 body，与 `runtime.c` 的真实定义产生 multiple definition 冲突；现只声明符号、不发射函数体。
- **落地文件**：`src/stdlib/core/core.suki`、`src/stdlib/concurrency/concurrency.suki`、`src/stdlib/system/system.suki`、`src/stdlib/objc/objc.suki`、`src/stdlib/CMakeLists.txt`、`src/compiler/main.cpp`、`src/compiler/codegen/IRGeneratorImpl.cpp`、`src/compiler/CMakeLists.txt`。
- **验证**：`import core` 程序 `stringLength/stringConcat/stringToInt` 正确输出 `5hello world123`（rc=0，无 IR 校验错误）；`core`/`concurrency`/`system`/`objc` 四个模块均可 `build` rc=0；`moduleTest/codegen/struct.suki` 回归通过。
- **遗留**：stdlib 内容目前仅为「协议声明 + 按值 String/Int 的安全 foreign 包装 + 命名便捷函数」骨架；指针类（Array/Dict/mutex/atomic/libc）API 留给各自 todo（C6/S3/E5）。另发现编译器既有 bug：整数字面量在比较里宽度不匹配（`icmp ne i32 %4, i64 0`），当前用「前缀/后缀判断返回 Int32 而非 Bool」规避，该 bug 应在后续统一修复。

### BLK-B 自包含 LLVM 后端 + LLD
- **现状**：`main.cpp` `commandBuild`/`commandRun` 调外部 `clang`（`SUKI_CLANG_DRIVER "clang"`）。
- **待实现**：在驱动中引入 LLVM C++ API（`llvm::Target` + `AsmPrinter` 直接产 `.o`；链接改用 `lld::elf/coff/wasm::link`）；新增 `-fuse-ld=lld`（默认开）、`--integrated-as`（默认开）。替换所有 `std::system("clang ...")`。
- **落地文件**：`src/compiler/main.cpp`、新增 `src/compiler/codegen/LLVMEmit.*`、`CMakeLists.txt`（链接 `lld`）。

---

## 阶段 1：类型系统收尾

### T1 多文件模块 / `import` 跨文件符号解析（§1.1 / §10.2）
- **现状**：`Parser.cpp:153/181` 解析 `ImportDecl`；`main.cpp:92` 仅 dump；`main.cpp:848/855` `build`/`run` 强制单文件；**零跨文件符号解析**。
- **待实现**：驱动一次接收同模块多文件，合并 AST 后跑 Sema；`ImportDecl` 解析为依赖模块符号表引用，跨模块查 `public` 符号（配合 T1 访问控制）。
- **落地文件**：`main.cpp`、`Sema.cpp`、`AST.h`。

### T2 `@enum(C)` C 兼容布局（§2.3）
- **现状**：`isCEnum` 仅在 `AST.h:114`/`Sema.cpp:608`/`Parser.cpp:495` **存储**，代码无任何读取/强制分支；原始枚举偶然降为 i64 使 `enum_c.suki` 通过，但无「禁关联值/union 重叠」语义。
- **待实现**：`Sema` 标记后禁止关联值（仅原始值或空载荷）；`TypeLayout` 增加 C-enum 分支（无 tag、仅 union、offset 0）；codegen 按 C 布局生成。
- **落地文件**：`Sema.cpp`、`TypeLayout.cpp`。

### T3 `Any` / `AnyObject` 装箱 + `as?`/`as!`/`is` 的 codegen（§2.8）
- **现状**：类型降 `i8*`（无 typeId/vtable）；`Sema` 对 `as?`/`is` 有检查（`Sema.cpp:2349-2357`），但 **`IRGeneratorImpl` 全文件零 `AsExpr`/`IsExpr` 分支** → `as?`/`as!` 无法降 IR。
- **待实现**：生成存在容器 `{payload 字节, typeId, vtable 指针}`；`as?`/`as!`/`is` 在 codegen 生成 typeId 检查 + 位转换（`genExistentialBox`/`genExistentialUnbox`）。
- **落地文件**：`Sema.cpp`、`IRGeneratorImpl.cpp`。

### T4 `String` 视图（§1.5）
- **现状**：插值已实现；`.unicodeScalars`/`.characters`/`.utf8`/`.utf16` 视图 **0 匹配**。runtime 仅有 `suki_str_utf8_count`/`utf8_get`。
- **待实现**：在恢复的 `src/stdlib` 中实现 `String` 内部 COW 存储（配合 BLK-A 与 S1）与四个 `Collection` 视图。
- **落地文件**：`src/stdlib/core/String.suki`（依赖 BLK-A）。

---

## 阶段 2：宏系统

### M1 重建 `MacroExpander` 全套（§5.6）
- **现状**：**彻底缺失**——`MacroExpander` 已删除且未重建；`AST.h:231` 仅残留 `MacroDecl` 节点；`Parser.cpp` 对 `MacroDecl`/`parseMacro` **零命中**（连 `@macro` 解析路径都不存在）；`main.cpp:161-162` 仅 dump。
- **待实现**：
  - 重建 `src/compiler/macro/MacroExpander.h/.cpp`；
  - `Parser`：`@freestanding(expression/declaration)`、`@attached(member/accessor/peer)`、`macro` 声明；在 Sema 前插入 MacroPass（AST→AST）；
  - **`SukiSyntax` API**：`SyntaxNode`/`SyntaxExpr`/`SyntaxDecl` 元编程 API（仅宏库作用域可见）；
  - **`#makeExpr` / `#makeDecl`**：字符串字面量 + `\(node)` unquote（仅接受 `SyntaxNode`，先源码文本替换后整段解析），配合卫生性；
  - **卫生性**：展开体新符号加 `__suki_macro_<scopeID>_` 前缀；`#unique("base")` 生成模块内唯一名；
  - **沙盒**：宏体禁止文件 I/O/网络/进程/系统调用，仅遍历/构造/重写 `Syntax` 节点与字符串拼接；
  - 调试：`sukic -expand-macros`、`sukipm build --verbose-macro`。
- **落地文件**：`src/compiler/macro/MacroExpander.*`、`Parser.cpp`、`Sema.cpp`（macro pass 钩子）、新增 `src/compiler/macro/SyntaxAPI.*`。

---

## 阶段 3：内存与系统编程

### S1 `Owned<T>` 与 COW 的 `.unique()` 独占性（§6.4）
- **现状**：`Owned<class>` 已禁（`Sema.cpp:992-1033`，`:1026-1031` 报错）；`RefKind::Owned` ABI 带 `i1 moved` flag（`TypeLayout.cpp:196-198`）；但 **COW `.unique()` 独占性零命中**，codegen 仍无真正「禁拷贝/释放所有权」逻辑。
- **待实现**：当 `T` 为 COW 类型（Array/String/Dictionary），构造与**每次移动**调用 `.unique()` 分离独占缓冲区；`Owned<[UInt8]>` 移动后不与旧副本共享存储；codegen 增加 `genMoveValue` ABI（不 retain、接收后释放所有权）。
- **落地文件**：`Sema.cpp`、`IRGeneratorImpl.cpp`、`src/stdlib`（COW 存储，依赖 BLK-A/T4）。

### S2 `unowned` 引用运行时（§6.1）
- **现状**：语法识别（`Parser.cpp:302-305/673-676/1949-1953`）；`RefKind::Unowned`（`Type.h:52`）；`TypeLayout.cpp:194` 与 weak 同处理为指针；但 **runtime 无 `suki_unowned_*`**（仅 weak）。
- **待实现**：runtime 新增 `suki_unowned_*`（非可选引用，访问时断言对象存活）；codegen 为 `[unowned self]` 捕获与 `unowned var` 生成 unowned 读取。
- **落地文件**：`runtime.c`、`IRGeneratorImpl.cpp`。

### S3 `MMapRegion` / `DynamicLibrary` / `sys` 模块（§8.3/8.4/8.5）
- **现状**：runtime/stdlib **0 匹配**（已删除）。
- **待实现**（恢复 `src/stdlib`）：`MMapRegion`（`mmap`/`munmap`）、`DynamicLibrary`（`dlopen`/`dlsym`/`dlclose` + `lookup`）、`sys`（`fork`/`exec`/`waitpid`/`open`/`read`/`write`/`ioctl`/`socket`/`bind`/`listen`/`accept`/`signal`/`sigaction`/`clock_gettime`/`nanosleep` 薄封装，失败抛 `SystemError`）。
- **落地文件**：`src/stdlib/system/**`、`runtime`（薄封装，依赖 BLK-A）。

### S4 裸机 `bare-metal` 支持（§8.7）
- **现状**：仅 `TargetInfo.cpp:51` 一句注释；无 `_start`/`@no_mangle`/`@panic_handler`/`@global_allocator` 解析或生成。
- **待实现**：`--target=*-none-*`/`bare-metal` 模式禁用 stdlib、仅 `core`；`Parser` 解析 `@no_mangle`/`@_cdecl`/`extern "C" fn _start()`/`@panic_handler`/`@global_allocator`；codegen 生成 `_start` 入口与 panic_handler 符号，不调用 ARC/runtime 初始化。
- **落地文件**：`main.cpp`、`Parser.cpp`、`IRGeneratorImpl.cpp`。

---

## 阶段 4：并发与异步

### C1 `async`/`await` 状态机（LLVM coroutine）（§7.2）
- **现状**：`Parser.cpp:333/342/882/1399` 仅置 flag；**Sema 无 await 强制**（grep `isAwait/await` 仅 `Sema.cpp:2285` 注释）；codegen `:1691-1693` `if(u->isTry||u->isAwait) return v;` **透传**；无 `@llvm.coro.*`、无状态机。
- **待实现**：Sema 标记 async 函数、识别 `await` 点（非 async 上下文用 await 报错）；codegen 用 `@llvm.coro.id/resume/suspend/end` 将 async 函数体 split 为 resume/suspend，每个 `await` 生成挂起点，async 返回包装为 `Future<T>`/`TaskHandle`；runtime 增加协程调度/挂起恢复。
- **落地文件**：`Sema.cpp`、`IRGeneratorImpl.cpp`、`runtime.c`（依赖 BLK-A/C3）。

### C2 `actor` 隔离与串行执行器（§7.4）
- **现状**：actor 当普通 class（`TypeDeclKind::Actor`、`TypeLayout.cpp:70/241` 同指针+rc；codegen `:2170/3474` 等同 class）；**无隔离/执行器**。
- **待实现**：Sema 标记 actor 方法默认 async、`nonisolated` 豁免；codegen 给 actor 实例挂串行执行器（消息队列），方法调用转 `await` 入队串行执行；可重入：`await` 后重查条件（生成重入安全桩）。
- **落地文件**：`Sema.cpp`、`IRGeneratorImpl.cpp`、`runtime.c`（actor 执行器，依赖 C1）。

### C3 `Task` / `TaskGroup` / `Future`（§7.1/§7.3）
- **现状**：仅词法关键字（`Task`/`Spawn`）+ `Task { }` 尾闭包解析（`Parser.cpp:738/1426/1554`）；**无线程池、无 Future/TaskGroup runtime**；stdlib 空。
- **待实现**：全局协作线程池（工作窃取）；`Future<T>.await(timeout:)` 超时抛 `TimeoutError`、响应父任务取消抛 `CancellationError`；`Task`（可取消/`isCancelled`/`withTaskCancellationHandler`/`Task.sleep`）、`TaskGroup`（`withTaskGroup` 并行子任务、子任务继承取消）。
- **落地文件**：`src/stdlib/concurrency/**`、`runtime.c`（线程池，依赖 BLK-A/C1）。

### C4 `Channel<T>` + `select` 语句（§7.5）
- **现状**：`select` 解析为无 subject 的 `SwitchStmt`（`Parser.cpp:1040-1057`）；`Channel` 仅关键字，无 `Channel<T>` 类型、无收发 codegen；`moduleTest` 零 select/channel 测试。
- **待实现**：
  - `AST.h` 新增 `SelectStmt`（区分 **发送** `valueExpr <- channel`、接收 `let binding <- channel`、**省略绑定** `case <- channel`、`default`）；
  - `Parser` 按**第 11 次修订方向**重写：`case 42 <- ch2`（发 42 到 ch2）、`case let msg <- ch1`（从 ch1 收并绑 msg）、`case <- ch`（仅等待就绪）；
  - Sema 校验 select 在 async 上下文；
  - codegen 多路等待 + 公平随机选一个就绪分支 + 非阻塞 `default` 路径；
  - stdlib：`Channel<T>` 有界（容量 + 背压 `.block/.dropNewest/.dropOldest/.throw`）/无界模式。
- **落地文件**：`AST.h`、`Parser.cpp`、`Sema.cpp`、`IRGeneratorImpl.cpp`、`src/stdlib/concurrency/**`（依赖 C1/C3）。

### C5 `@MainActor` / `@executor(X)` 自定义执行器（§7.2）
- **现状**：**0 匹配**。
- **待实现**：Sema 对函数/类型上的 `@MainActor`/`@executor(X)` 属性 → 调用点强制在对应执行器运行（复用 C2 actor 执行器机制）；跨执行器调用插入 hop（状态保存/恢复）。
- **落地文件**：`Sema.cpp`、`IRGeneratorImpl.cpp`（依赖 C2）。

### C6 `Atomic` / `Mutex` / `RWLock` / `Semaphore` / `Condition` / `DispatchQueue`（§7.6）
- **现状**：runtime 有底层 `SukiMutex`/`suki_mutex_*`（`runtime.h:131-137`）、`suki_atomic_*`（`runtime.h:139-141`，`runtime.c:611-637`）；但**无 Suki 级 `Atomic<T>`/`Mutex`/`RWLock`/`DispatchQueue` 类型**（stdlib 空）。
- **待实现**：stdlib 基于 pthread + LLVM atomic intrinsics 封装上述类型。
- **落地文件**：`src/stdlib/concurrency/**`（依赖 BLK-A）。

---

## 阶段 5：错误处理与互操作

### E1 `Result<T, E>`（§9.4）
- **现状**：**0 匹配**；stdlib 空。
- **待实现**：`Result<T, E: Error>` 枚举（`.success`/`.failure`）+ 常用方法。
- **落地文件**：`src/stdlib/core/Result.suki`（依赖 BLK-A）。

### E2 `Error` / `CustomStringConvertible` / `localizedDescription`（§9.3）
- **现状**：仅 `TypeKind::Error` 占位（`Type.h:32`）；0 实现；stdlib 空。
- **待实现**：`Error` 空协议 + 默认 `localizedDescription`（`String(describing: self)`）；`CustomStringConvertible` 协议。
- **落地文件**：`src/stdlib/**`（依赖 BLK-A/T4）。

### E3 `extern "stdcall"` 调用约定（§13.1/§13.3）
- **现状**：`@_cdecl("name")` 已实现（`Parser.cpp:212-224/341`、`AST.h:163`、`IRGeneratorImpl.cpp:151-153`）；**`stdcall` 调用约定 0 匹配**。
- **待实现**：`extern` 声明增加调用约定字段（C/stdcall → LLVM `callcc`/函数 attribute）；解析 `extern "stdcall" func ...`。
- **落地文件**：`Sema.cpp`、`Parser.cpp`、`IRGeneratorImpl.cpp`。

### E4 `#selector`（§13.2.4）
- **现状**：**0 匹配**。
- **待实现**：`Parser`/`Sema`：`#selector(method)` → 展开为 `sel_registerName("method")` 调用（依赖 E5 ObjC 运行时预声明）。
- **落地文件**：`Parser.cpp`、`Sema.cpp`（依赖 E5）。

### E5 Objective-C 互操作（§13.2）
- **现状**：**0 匹配**（无运行时预声明/消息发送）。**注意**：规范第 11 次明确——ObjC 消息语法属预留可选特性，**当前未定义、未实现**；现阶段唯一途径是 §13.2 的 `objc_msgSend` 手动互操作。
- **待实现**：Sema 自动注入 `objc_msgSend`/`sel_registerName`/`objc_getClass`/`objc_retain`/`objc_release` 等 extern 预声明；codegen 生成 `objc_msgSend` 调用 IR；ObjC 对象复用现有 ARC（weak/unowned）。
- **落地文件**：`Sema.cpp`、`IRGeneratorImpl.cpp`（依赖 E3）。

### E6 `@convention(c)` / `@convention(stdcall)` 函数类型值（§13.6）★第 11 次新增
- **现状**：**全仓零命中**（无 `convention`/`stdcall`/`CallingConv` 处理）。
- **待实现**：`Sema` 解析 `@convention(c)`/`@convention(stdcall)` 作为**函数类型值的属性**（区别于 E3 的 `extern` 声明约定），生成带相应 LLVM calling convention 的函数指针类型；与 `@_cdecl`/extern 组成完整 C ABI 映射。
- **落地文件**：`Sema.cpp`、`Parser.cpp`、`IRGeneratorImpl.cpp`、`Type.h`（依赖 E3）。

### E7 字符串显式桥接 `withCString` / `.cString`（§13.2.2）★第 11 次新增
- **现状**：**全仓零命中**（runtime 无相应导出）。
- **待实现**：规范规定只有字符串**字面量**隐式桥接为 NUL 结尾 UTF-8 `UnsafePointer<Int8>`；运行期 `String` 变量需显式 `withCString { ... }` 或 `.cString` 桥接。在 runtime/stdlib 实现临时 C 字符串导出（生命周期由闭包/调用点管理）。
- **落地文件**：`runtime.c`、`src/stdlib/core/String.suki`（依赖 BLK-A/T4）。

### E8 `Duration` 类型（§7.3）★第 11 次新增
- **现状**：**全仓零命中**（仅无关词 "for the duration of"）。
- **待实现**：`Duration` 类型，工厂 `.seconds(_:)`/`.milliseconds(_:)`/`.microseconds(_:)`；`Task.sleep`、`future.await(timeout:)`、定时器统一接收 `Duration`；明确禁止 `5.seconds` 写法。
- **落地文件**：`src/stdlib/core/Duration.suki`（依赖 BLK-A）。

### E9 `Awaitable` 协议 + `await future` 脱糖（§7.3）★第 11 次新增
- **现状**：**全仓零命中**。
- **待实现**：定义 `protocol Awaitable { associatedtype Value; func waitForValue() async throws -> Value }`；`Future<T>` 遵循 `Awaitable`；`await future` 脱糖为 `try await future.waitForValue()`；`waitForValue()` 在调用处挂起当前 async 任务并注册 continuation，后台线程完成后续唤醒；挂起点检查 `Task` 取消，已取消抛 `CancellationError` 并向后台计算发取消请求。
- **落地文件**：`src/stdlib/concurrency/**`、`Sema.cpp`（脱糖，依赖 C1/C3）。

---

## 阶段 6：后端与工具链

### B2 CLI 选项补全（§14.1.1–14.1.3）
- **现状**：仅 `--target=`/`-o`/`--dump-ast` + 命令 `lex/parse/check/emit-ir/build/run`；缺 `--list-targets`/`-S`/`--emit-llvm`/`-c` 多文件/`--sysroot`/`-O*`/`--incremental`；`-c` 多文件被 `main.cpp:848/855` 拒绝。
- **待实现**：`--list-targets`（遍历 `llvm::TargetRegistry`）；`-S`（AsmPrinter 汇编文本）；`--emit-llvm`（`.ll`/`.bc`）；`-c` 多文件分别编译为 `.o`（改为遍历按模块聚合）；`--sysroot` 透传 LLD；`-O0/-O1/-O2/-Os/-O3` 映射 `PassBuilder`/`OptLevel`（配合 debug/release/size，§10.4）；`--incremental`（`.build/cache` 按源内容 hash + 依赖图缓存，依赖 BLK-B/T1）。
- **落地文件**：`main.cpp`（依赖 BLK-B）。

### B3 DWARF 调试信息（§14.3）
- **现状**：**0 匹配**（无 `DIBuilder`/`DWARF`）。
- **待实现**：`IRGeneratorImpl.cpp` 用 `llvm::DIBuilder` 为每个函数/变量/类型生成 `!DISubprogram`/`!DILocalVariable`/`!DILocation`；泛型实例化类型名纳入 DWARF。
- **落地文件**：`IRGeneratorImpl.cpp`（依赖 BLK-B）。

### B4 `sukipm` 完善（§11/§14.6）
- **现状**：`tools/sukipm/` 有 `main.cpp`/`Manifest`/`DependencyResolver` 源码，但：`build`/`test` 的 `sukic` 调用语法错误（`sukipm/main.cpp:125/152`，应为 `sukic build ... -o ...`/`sukic run`）；`add` 打印 TODO（`:175`）；`publish`/`docs` "not yet implemented"（`:221-224`）；`DependencyResolver::resolve` 仅复制依赖、版本/范围解析全 TODO（`DependencyResolver.cpp:9-53`）。
- **待实现**：修正 CLI 调用；实现 `add`/`publish`/`docs`；`DependencyResolver` 接入 PubGrub 版本/范围解析；`sukipm docs` 从 `///` 注释抽取生成 HTML/Markdown（复用 `suki-doc`，§14.6）。
- **落地文件**：`tools/sukipm/**`。

### B5 `suki-lsp` 真实实现（§14.2）
- **现状**：**无法编译**——引用了不存在的编译器 API（`CompilationUnit`/`Decl`/`DeclKind`/`lexAll()`/`parse()`/`DiagnosticEngine::diagnostics()`/`hadErrors()`/`printAll()`，`LSPServer.cpp:269-272/343-346`），与真实 `Lexer.h:20,23`（`tokenizeAll()`）、`Parser.h:19,21`（`parseModule()` 返回 `NodeList`，AST 为 `Node`/`NodeKind`）、`DiagnosticEngine.h:13-56`（无 `diagnostics()`）不符；`hover` 返回 null、completion 仅静态关键字。
- **待实现**：对接真实编译器 API（改用 `tokenizeAll`/`parseModule`/`Node`/`NodeKind`/`DiagnosticEngine` 真实接口）；补全补全/跳转定义/重构/错误提示（复用 Sema 诊断）。
- **落地文件**：`tools/suki-lsp/**`。

### B6 `suki-fmt` 真实实现（§14.4）
- **现状**：**无法编译**——`Formatter` 引用不存在的 AST（`CompilationUnit`/`Decl`/`DeclKind`/`Stmt`/`Expr`/`Pattern`/`GenericParam`，`Formatter.cpp:10-229`），真实 AST 为 `Node`/`NodeKind`；二元表达式一律输出 `" ? "` 占位（`:156-163`）。
- **待实现**：改用真实 AST（`Node`/`NodeKind`）做格式化；按 §1.9 风格规则（4 空格缩进、大括号位置、运算符空格、命名约定），支持 `.suki-fmt.json` 配置。
- **落地文件**：`tools/suki-fmt/**`。

> **备注**：`suki-doc`（`tools/suki-doc/`）经审计为**真实可用**（纯正则 `///` 抽取 + HTML/Markdown 生成，独立可跑），不列入未实现清单。

---

## 实施顺序建议（依赖关系）

1. **阶段 0（BLK-A / BLK-B）** → 必须先做，否则后续大量特性无法编译/链接/验证。
2. **阶段 1（T1–T4）** → 类型系统收尾；T1 多文件模块是访问控制/增量编译前提。
3. **阶段 2（M1）** → 独立，建议在 Sema 管线稳定后做。
4. **阶段 3（S1–S4）** → S1 依赖 BLK-A/T4（COW）；S2/S3 依赖 BLK-A；S4 独立。
5. **阶段 4（C1–C6）** → C1 async 状态机是 C2/C3/C4/C5 的共同底座；C3/C4 依赖 BLK-A；C6 依赖 BLK-A。
6. **阶段 5（E1–E9）** → E1/E2 依赖 BLK-A；E3 是 E5/E6 前提；E4 依赖 E5；E6/E7/E8/E9 为第 11 次新增，E8/E9 与 C1/C3 强相关。
7. **阶段 6（B2–B6）** → B2/B3 依赖 BLK-B；B4/B5/B6 可并行，但 B5/B6 依赖真实编译器 API 稳定（阶段 1 完成后）。

> 所有含 `.suki` 标准库的实现（T4、S1、S3、C3、C4、C6、E1、E2、E7、E8、E9）均**依赖 BLK-A 先重建 `src/stdlib` 骨架**。
