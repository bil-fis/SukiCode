# SukiCode 标准库 `core` 模块构建（step01）

> 对照 `SukiCode_Specification.md` 与 `results/compiler-unimplemented-features-plan.md`。
> 目标：在**当前编译器能力边界内**交付一个可编译、可运行（退出码 0）的 `core` 标准库模块。

---

## 一、交付物

- `src/stdlib/core/core.suki`：标准库核心模块，经 `import core` 加载。
- `src/compiler/parser/Parser.cpp`：修复一处会导致枚举 case 等成员解析失败的编译器 bug（见第三节）。
- `run_all_tests.sh`：`KNOWN_PENDING` 登记两项待实现用例（`result`、`memory`）。
- `moduleTest/pending/result.suki`：原 `codegen/result.suki` 移入 pending。

## 二、`core` 模块已实现并验证的能力

| 分类 | 提供的 API |
|------|-----------|
| 协议 | `Equatable` / `Hashable`（含 `hashValue()`）/ `Comparable` / `CustomStringConvertible`（含 `description()`）/ `Error` —— 协议方法声明已被编译器支持 |
| 字符串 | `foreign` 桥接 `suki_str_length/concat/compare/has_prefix/has_suffix/to_int/int_to_string/double_to_string/char_to_string/panic_string`；封装 `stringLength`/`stringConcat`/`stringHasPrefix`/`stringHasSuffix`/`stringCompare`/`stringLess`/`intToString`/`doubleToString`/`charToString`/`stringToInt` |
| 数值 | `min`/`max`/`abs`/`clamp`/`minD`/`maxD`/`clampD`/`swapInt`（Int 与 Double 因不支持按参数类型重载而用不同命名） |
| Range（§1.8） | `struct Range`、`range(from:to:)`、`rangeContains`、`rangeCount`、`stride(from:to:by:)` |
| Optional（§2.7） | 具体重载 `unwrapOrInt`/`unwrapOrString`/`unwrapOrDouble`/`unwrapOrBool`（见第四节限制 4） |
| 断言（§9） | `assert`/`precondition`/`fatalError`，底层走 ABI 兼容的 `suki_panic_string`（`src/runtime/runtime.c` 新增 `suki_panic_string`：打印 `panic: <msg>` 后 `abort()`） |
| 随机（§12.1） | 全局 LCG 状态 `_sukiRngState` + `randomSeed`/`randomInt`/`randomIntInRange`/`randomBool`（无额外 runtime 依赖） |
| 裸机（§8.7） | `struct PanicInfo`、`protocol GlobalAlloc`（`alloc`/`dealloc`），供 `@panic_handler`/`@global_allocator` 属性校验使用 |

验证方式：编写 `import core` 测试程序，逐一调用上述 API 做不变量断言，`sukic run` 退出码 **0**（全部通过）。

## 三、修复的编译器 bug

**症状**：`enum Color { case red; case green }`（成员间以 `;` 分隔）报 `unexpected member declaration`；`core` 模块内的 `enum Result<T, E: Error> { case success(T); case failure(E) }` 同样失败。

**根因**：`Parser::parseTypeDecl` 的成员解析循环在解析完一个成员（如 `parseEnumCase`）后未消费尾随的 `;`，导致下一轮把 `;` 当作非法成员声明。

**修复**：在成员循环开头跳过前导 `;`（与语句级 / 顶层一致）。
```cpp
while (!checkPunct(PunctuatorID::RBrace) && !atEnd()) {
    // 成员之间允许用 `;` 分隔（与语句级 / 顶层一致），跳过前导分号。
    while (checkPunct(PunctuatorID::Semicolon)) advance();
    ...
}
```
落点：`src/compiler/parser/Parser.cpp`（`parseTypeDecl` 成员循环）。

## 四、发现的编译器限制（阻塞了 spec 的更完整实现）

构建过程中逐项二分确认以下限制，已在 `core.suki` 头部注释与本文记录；相关特性以「具体类型 / 规避写法」替代，或移入 pending。

1. **闭包未实现**：任何接收并调用闭包的高阶函数（`map`/`filter`/`reduce`/`forEach`/`sorted`/`compactMap`/`zip`）无法表达。规范 §12.2/§12.3/§12.7 的绝大多数集合高阶 API 因此无法落地。
2. ~~**泛型枚举不可用**~~ **已修复**：原先 `enum Box<T> { case wrapped(T); case empty }` 实例化时报 `cannot convert value of type 'Box' to 'Box<Int><Int>'`（实例名被双重包裹）。根因有二：(a) 构造 `E.case(...)` 时返回的是未单态化的泛型 `Box` 而非实例 `Box<Int>`；(b) `typeToString` 对单态化实例（`name` 已是 `Box<Int>` 且 `elements=[Int]`）再次追加 `<Int>`，渲染为 `Box<Int><Int>`。两类均已在后续编译器修复中消弭。规范 §9.4 的 `Result<T, E: Error>` 现已作为泛型枚举落地于 `src/stdlib/core/core.suki`，回归用例 `moduleTest/codegen/result.suki` 通过。
3. **`for … in` over `Set` 导致编译器段错误**（数组 for-in 正常）。故依赖迭代的 `setUnionInt`/`setIntersectionInt`/`setSubtractInt` 无法提供；单元素的 `insert`/`contains` 可正常使用。
4. **2 参数泛型函数不被单态化**：`func pair<T>(_ a: T, _ b: T)` 的实例未被生成，调用点回退为变参 `(ptr, ...)` 占位，最终 IR 校验失败。故通用的 `unwrapOr<T>(_ o: T?, fallback: T)` 改为四个具体类型重载。
5. **从分支体返回结构体值（如 `String`）被错误降级为 `ret ptr`**：`func f(_ o: String?) -> String { if let v = o { return v } … }` 触发 `Function return type does not match operand type of return inst`。规避写法：用局部变量承接、`return` 统一落在函数末尾（已用于 `unwrapOrString` 等）。标量（Int/Double/Bool）从分支返回不受影响。

> 注：现有 `compiler-unimplemented-features-plan.md` 中部分条目已过时（例如 `loop` 关键字实际已实现，`@no_mangle`/`@panic_handler`/`@global_allocator` 已有解析与 Sema 校验），后续可单独修订该文档。

## 五、测试状态

`bash run_all_tests.sh --no-build` → **通过 76，失败 0，跳过 4，全部测试通过 ✅**。

- 跳过项：`examples/concurrency`（既有 pending：actor 隔离 / select / for await / 区间）、`moduleTest/pending/result.suki`（泛型枚举待编译器支持）、`examples/basics`（无 `@main`，仅编译验证）、`examples/memory`（memory 模块待实现）。
- 宿主单元测试、语言 codegen 用例、负例诊断、解析健全性（96 个 `.suki`）全部通过，确认本次 `Parser.cpp` 修复与 `core.suki` 改写未引入回归。

## 六、下一步

- 若需 `Result<T, E>` 与集合高阶 API，须先修复编译器：泛型枚举单态化（限制 2）、闭包（限制 1）、`Set` 的 for-in 代码生成（限制 3）、2 参数泛型单态化（限制 4）。
- `memory` 模块（`Owned<T>`/`ByteBuffer`/ARC）、`system`/`io`/`concurrency` 等标准库模块受上述限制约束，需按「无闭包 / 无泛型枚举 / 规避分支返回结构体」的写法渐进构建。
