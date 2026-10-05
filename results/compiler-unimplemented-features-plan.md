# SukiCode 编译器「原生功能未实现」清单与实施计划

> 对照规范 `SukiCode_Specification.md`（v1.0.0）。审计依据：`src/compiler` 的 Lexer / Parser / Sema / codegen 实现、`moduleTest/` 测试覆盖，以及 `run_all_tests.sh` 的 `KNOWN_PENDING`。
> `moduleTest/pending/` 当前为空，因此「未实现」由源码与测试推断得出。

---

## 一、完全未实现

### 1. 宏系统（§5.6）——本期实现目标
- **M1（已完成 ✅）**：`@freestanding(expression)` 端到端可用——宏声明解析、`#makeExpr` 模板、`\(arg)` unquote（AST→源码递归序列化 + 运算符实参括号保护）、重解析 + 就地替换、嵌套宏调用、`sukic --expand-macros` 诊断。详见 `moduleTest/codegen/macros.suki`。
- **M2（已完成 ✅）**：`@freestanding(declaration)` 顶层/嵌套声明位宏、`@attached(member)` 成员注入、`#unique` 卫生性唯一标识符、`MacroExpansionTooComplex` 复杂度上限、展开错误携带子诊断文本。详见第 6.5 节与 `moduleTest/codegen/macros_m2.suki`。
- 仍缺失（M3）：沙盒（`SukiSyntax` 受限 API 解释执行，当前 M1/M2 用「源码文本替换 + 重解析」合规实现）、`@attached(accessor|peer)`、`import SukiSyntax` 元编程 API、卫生性重命名闭环（M1/M2 表达式/成员宏通常无新绑定，unquote 文本天然保留调用方作用域，`#unique` 提供唯一名）。
- 现状：M1/M2 之前仅有 `MacroDecl` AST 节点与 `main.cpp` 的 pretty-print；现已补齐 Parser / Sema 展开 pass，codegen 复用现有逻辑。

### 2. C / ObjC 互操作的关键项（§13）
- `@convention(c|stdcall)`（§13.6）：未实现。
- `unsafeBitCast`（§13.2.2）：未实现。
- `#selector`（§13.2.4）：未实现。
- 已实现：`extern "C"` 声明、`@_cdecl` 导出、`objc_*` 运行时函数已规划但 ObjC 消息语法为预留可选（§15.5 明确未实现）。

### 3. 规范要求的原生类型缺失（§6 / §7 / §8 / §12）
- `UnsafePointer` / `UnsafeMutablePointer` / `UnsafeBufferPointer` 系列（§6.5 / §8.1）：规范要求泛型裸指针类型，但 stdlib 中未定义，仅有非泛型、固定 `Int` 地址的 `RawPtr`（`src/stdlib/memory.suki`）。
- `ThreadPool` / `DispatchQueue`（§7.1 / §7.6 / §12.3）：作为命名类未实现（仅 `Future`/`Atomic`/`Mutex` 已注册为内建；`Task`/`TaskGroup`/`Channel`/`select`/`for await` 已可用）。
- `ChannelPool`（§6.2 / §7.5）：规范提及但编译器无定义，仅 `Channel`。
- `Result<T, E>`（§9.4）：无类型、无 codegen，仅 `Error` 协议。

### 4. 控制流（§1.7）
- `loop` 无限循环关键字：`Token.h` 关键字表中**无 `loop`**（仅 `while`/`for`/`repeat`），完全未实现。
- 标签 break（`outer: loop { … break outer }`）：`BreakStmt` 有 `label` 字段，但 codegen 仅跳最内层，标签语法与跳转未实现。

### 5. 属性（§8.7 / §7.2）
- `@no_mangle`、`@panic_handler`、`@global_allocator`、`@executor`、`@MainActor`：均无解析 / 语义 / codegen。仅 `@main` 与 `@enum(C)` 被处理。

### 6. 其它独立项
- `@autoclosure`（§3.5）：仅 `ClosureExpr::isAutoclosure` AST 占位字段，无实现。
- 泛型 `typealias`（§2.10）：`collectTypeAlias`（`Sema.cpp:1192`）忽略 `genericParams`，仅非泛型别名可用。

---

## 二、部分实现（有骨架但行为不完整）

- **actor 隔离（§7.4）**：仅「单次自旋锁 + async 边界」（`IRGeneratorImpl.cpp` 的 actor 成员调用），串行执行器队列与重入（reentrancy）语义未建模。
- **条件编译 `#if`（§10.3）**：`Lexer` 支持 `#if/#else/#endif` 与 `-D` 宏，但条件求值仅 `defined()`/布尔，**无 `os()`/`arch()` 函数**（测试 `cond_compile.suki` 仅用 `#if false`）。
- **`let` 确定性赋值分析（§1.3）**：延迟初始化规则（所有路径恰好赋值一次、禁止未赋值读取）**未实现**（全仓无 definite-assignment 遍）。
- **`import` 跨模块 / 访问控制（§10.1）**：当前单模块模型，`import` 基本占位；`public`/`internal`/`fileprivate` 与 `internal` 等同可见，仅 `private` 真正强制。
- **C 互操作**：`extern`/`@_cdecl` 已实现，但 `@convention`/`unsafeBitCast`/`#selector` 未实现。

---

## 三、已知 pending 示例（组合级缺口）
- `run_all_tests.sh` 的 `KNOWN_PENDING` 登记 `examples/concurrency.suki`，标注仍缺「actor 隔离 / select / for await / 区间 `0..<100`」组合。但 `select.suki`、`for_await.suki`、`actor_isolation.suki`、`channel.suki`、`task_group.suki`、`with_task_group.suki` 等**独立 codegen 测试均通过**——特性单项可用，完整 showcase 组合尚未通过。

---

## 四、已确认实现（供对照，不全列）
泛型（含 `where`/associatedtype/`some P`）、协议（继承/默认实现/关联类型）、extension、subscript、willSet/didSet、computed property、`Optional` 与可选链（`?.`/`!`/`??`）、非泛型 typealias、tuple（含标签）、async/await（线程+Future 模型）、Task/TaskGroup/withTaskGroup/for await、select、Channel、Future/Atomic/Mutex 内建、`extern`/`@_cdecl`、内联 `asm`、`unsafe` 块约束、`move`/`Owned<T>`（class 拒绝已修）、guard/repeat-while/fallthrough/switch+where+区间、`Range`+for-in、字符串插值/raw/多行、继承 override/final/required/convenience/super.init、闭包捕获 `[weak/unowned]`/`@escaping`/尾随/箭头、`enum` raw/关联值/`@enum(C)`、throws/do-catch/try?/try!/Error、`@main`/`@enum`。

---

## 五、建议实施优先级
1. `loop` 关键字（§1.7，纯语法+codegen，影响裸机 `_start`）
2. `#if os()/arch()`（§10.3，条件编译补全）
3. `Result<T,E>`（§9.4，错误处理闭环）
4. `UnsafePointer` 系列（§6.5，替代当前 `RawPtr`）
5. `@no_mangle` / `@panic_handler` / `@global_allocator`（§8.7，裸机必需）
6. **宏系统（§5.6，工作量最大，详见第六节）**
7. 泛型 `typealias`、`@autoclosure`、标签 break、确定性赋值分析
8. `@convention`/`unsafeBitCast`/`#selector`、ThreadPool/DispatchQueue/ChannelPool

---

## 六、宏系统（§5.6）实施设计

### 6.1 目标里程碑
- **M1（本期）**：词法 / 语法 / AST 基础 + `@freestanding(expression)` 宏端到端可用。
  - `macro` 关键字声明；`@freestanding(expression)` / `@freestanding(declaration)` 属性。
  - 宏体中的 `#makeExpr("…")` / `#makeDecl("…")` 原语，支持 `\(node)` unquote。
  - 调用点 `#name(args)` 解析为宏展开表达式 / 声明。
  - 展开 pass（在 Sema 之前）：按名字查找宏声明 → 取模板字符串 → 将 `\(argName)` 替换为对应实参的**原始源代码文本** → 整体重新解析为 AST → 就地替换（freestanding(expression) 替换表达式节点，freestanding(declaration) 替换声明列表）。
  - 卫生性：展开体内新建绑定名加 `__suki_macro_<scopeID>_` 前缀；unquote 嵌入节点保留调用方作用域。
  - `#unique("base")` 生成 `__suki_unique_<module>_<base>_<counter>` 标识符。
  - `sukic -expand-macros <file>` 打印展开后源码 / AST。
- **M2（已完成 ✅）**：`@attached(member|accessor|peer)` 宏；错误报告指向模板子位置；`MacroExpansionTooComplex` 上限。详见 6.5 节。
- **M3**：沙盒（`SukiSyntax` API 受限环境），真正的编译期解释执行而非字符串重解析（当前 M1 用「源码文本替换 + 重解析」满足规范边界规则，是合规实现）。

### 6.2 关键设计决策
- **展开时机**：宏展开在 parse 之后、Sema 之前完成，展开产物是普通 AST，复用现有 Sema / codegen，不污染后端。
- **quote/unquote 形式化**（遵循 §5.6）：`#makeExpr(s)` 的 `s` 必须是字符串字面量；编译器以 `\(node)`（node 为宏参数或宏体构造的 SyntaxNode）做 unquote——将 node 的**原始源代码文本**替换进模板，最后一次性解析整段字符串为单个语法树。unquote 只接受 `SyntaxNode`，不接受运行期值，从机制上防注入。
- **卫生性**：每个宏调用点分配唯一 `scopeID`；展开体内部新引入的绑定名重写为 `__suki_macro_<scopeID>_<origName>`；unquote 嵌入的节点（来自调用方）保留原始作用域、不被重命名。
- **错误信息**：指向 `#makeExpr` 模板中的具体字符位置（子位置），需保留原始名与源位置映射供 LSP。

### 6.3 集成点（待实现时落点）
- `Lexer`：`macro` 关键字；`#makeExpr`/`#makeDecl`/`#unique` 作为 `#`-前缀宏调用 token（区别于 `#if`/`#define` 预处理指令，由上下文区分）。
- `AST.h`：`MacroDecl`（已占位，需补字段）、`MacroExpansionExpr`、`MacroExpansionDecl`；`MakeExprExpr` / `MakeDeclExpr` 承载模板字符串与 unquote 节点列表。
- `Parser`：`parseMacroDecl`、`parseMacroExpansion`（表达式位与声明位）、宏体解析（复用普通表达式/声明解析）。
- `Sema`：在 `collectDecls` 之后、`resolveDecls` 之前插入 `expandMacros()` pass；维护 `scopeID` 计数器与 `#unique` 计数器。
- `main.cpp`：`-expand-macros` 选项；展开 pass 调用位置。

---

### 6.5 M2 实现状态（已完成 ✅）

> 状态：宏系统 M2 已实现并验证通过。回归套件 66 PASS / 1 FAIL（仅 `examples/memory`，本任务前既有的未实现特性失败，与宏无关）。新增用例 `moduleTest/codegen/macros_m2.suki` 覆盖全部 M2 形态。

#### 6.5.1 已落地能力
- **`@freestanding(declaration)` 声明位宏**：宏体 `return #makeDecl("…\n…")`（多声明以字面量换行分隔）。调用点 `#name(args)` 作为顶层/块内表达式语句承载，由 `expandDeclList` 列表级 pass 替换为重解析得到的声明列表（顶层、函数体 `BlockStmt.statements`、类型 `members`、各控制流语句体均递归覆盖）。
- **`@attached(member)` 成员宏**：被注解类型（`@MacroName struct/enum/class/...`）在 `expandMacros` 阶段被识别属性匹配，宏展开产物拼接到 `TypeDecl.members`。模板内 `\(self)` 引用被注解类型名（如 `Widget` → `func typeName() -> String { return "Widget" }`）。
- **`#unique` 卫生性原语**：模板内 `\(#unique)` 展开为全局唯一标识符 `__suki_unique_<n>`，用于宏引入的稳定唯一绑定名（避免与调用方作用域冲突）。
- **`MacroExpansionTooComplex` 复杂度上限**：`expandBudget_`（默认 200000 节点）在每次展开后按产出节点数递减，超限即报 `macro expansion exceeded MacroExpansionTooComplex limit` 并终止该次展开（防失控/自引用）。
- **展开错误子位置/子诊断透传**：重解析失败（`parseModule`/`parseExpression` 报错的 `subDiags`）经 `DiagnosticEngine::lastErrorMessage()` 透传，例如 `error: macro 'bad' produced invalid declaration expansion: <子诊断>`。

#### 6.5.2 关键实现决策（相对 6.2/6.3 的落地调整）
- **列表级展开 `expandDeclList` + `expandDeclListContainers`**：声明位宏调用是 `ExprStmt -> CallExpr(#name)`，单个 `CallExpr` 节点无法就地替换为多条声明，故在列表层面把该 `ExprStmt` 整体替换为展开声明列表，并递归处理嵌套块/类型成员。表达式位宏仍由 `expandInNode` 的 `CallExpr` 替换路径处理（M1）。
- **模板抽取前移**：`collectMacro` 注册时即调用 `extractTemplate`，使 `expansionKind_`（expr/decl）在 gate 判断（是否声明位宏、是否 attached member）之前就绪，避免「尚未抽取 → 判断为假 → 不展开」的竞态。
- **`selfType` 透传**：`buildMacroCode(md, call, selfType)` 统一构造展开源码；`selfType` 非空时 `\(self)`/`\(Self)` 解析为被注解类型名（`Self` 关键字被词法规范为小写 `self`，故两者都匹配）。
- **顶层 `#name(...)` 解析**：`parseModule` 顶层循环在 `parseDecl` 前识别 `#` + `Ident` + `(` 形式，作为表达式语句承载，交给 Sema 展开（规范 5.6 的顶层声明宏写法）。
- **`walkChildren` 首参数改为 `Node*`**（原 `NodePtr&`）：便于 `countNodes` 等静态/非持有遍历复用，避免 `Node*`→`NodePtr&` 转换失败。

#### 6.5.3 涉及文件与落点
- `src/compiler/ast/AST.h`：`MacroDecl::expansionKind_`（expr/decl）。
- `src/compiler/parser/Parser.cpp`：`parseModule` 顶层识别 `#name(...)` 宏调用；`parseMacroDecl` 已落 M1。
- `src/compiler/diag/DiagnosticEngine.{h,cpp}`：`lastErrorMessage()` 取最近错误文本（子诊断透传）。
- `src/compiler/sema/Sema.{h,cpp}`：`buildMacroCode` / `expandDeclTemplate` / `tryExpandDeclMacro` / `tryExpandAttached` / `expandDeclList` / `expandDeclListContainers` / `isDeclMacroCall` / `countNodes`；`expandMacros` 注入 attached 成员并运行列表级展开；`MacroExpansionTooComplex` 预算与 `#unique` 计数器。
- `moduleTest/codegen/macros_m2.suki`：M2 回归（freestanding 声明宏生成 `GenPoint`；`User` 注入 `id`+`uid`；`Widget` 注入 `typeName`→`"Widget"`；`#unique` 生成全局变量）。

#### 6.5.4 验证方式
- 端到端：`sukic run moduleTest/codegen/macros_m2.suki` → 退出码 0（5 项断言全过）。
- 展开诊断：`sukic check moduleTest/codegen/macros_m2.suki --expand-macros` 可见 `Struct GenPoint`、`User(members=3)`、`Widget.func typeName`、`Var __suki_unique_1` 等展开产物。
- 错误子位置：`sukic check <含非法展开的宏>` → `error: macro 'bad' produced invalid declaration expansion: <子诊断>`（不崩溃）。
- 回归：`bash run_all_tests.sh --no-build` —— 仅 `examples/memory`（既有失败，与宏无关）失败；其余全绿。

#### 6.5.5 M3 待办（未实现）
- 沙盒（`SukiSyntax` 受限 API，编译期解释执行而非字符串重解析）——当前 M1/M2 的字符串重解析实现合规且端到端可用。
- `@attached(accessor|peer)` 展开位置（当前仅 `member`）。
- `import SukiSyntax` 元编程 API。
- 卫生性重命名闭环（以 `scopeID` 前缀重写展开体内新绑定名）；`#unique` 已提供唯一名，闭环重命名留待 M3。
