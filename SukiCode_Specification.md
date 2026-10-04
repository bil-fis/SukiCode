# SukiCode 语言规范

**版本 1.0.0**
**设计修改第 13 次（语义收敛修订）**

**设计目标**：融合 Swift、TypeScript 与 Objective-C 语法优势的现代系统编程语言，通过 AOT 编译、确定性 ARC 内存管理与 Pool/Collection 资源抽象，提供 C 级性能与高度可预测的运行时行为，同时支持全部主流操作系统特性。

---

## 目录

1. [语言基础](#1-语言基础)
2. [类型系统](#2-类型系统)
3. [函数与闭包](#3-函数与闭包)
4. [面向对象与协议](#4-面向对象与协议)
5. [泛型与元编程](#5-泛型与元编程)
6. [内存管理](#6-内存管理)
7. [并发与异步](#7-并发与异步)
8. [系统编程与底层操作](#8-系统编程与底层操作)
9. [错误处理](#9-错误处理)
10. [模块、访问控制与编译](#10-模块访问控制与编译)
11. [包管理器：SukiPM](#11-包管理器sukipm)
12. [标准库概览](#12-标准库概览)
13. [与 C / Objective-C 互操作](#13-与-c--objective-c-互操作)
14. [工具链与项目结构](#14-工具链与项目结构)
15. [设计权衡与哲学](#15-设计权衡与哲学)

---

## 1. 语言基础

### 1.1 源文件与模块

SukiCode 源文件使用 `.suki` 扩展名。每个文件属于一个模块，模块名在文件首行声明：

```swift
module MyApp
```

同一模块可由多个文件组成，无需重复模块声明（除入口文件外）。模块名需与文件系统目录及包清单一致。入口文件（包含 `@main` 属性的文件）可以省略 `module` 声明。

### 1.2 注释

```swift
// 单行注释
/* 多行注释 */
/// 文档注释（支持 Markdown），用于生成文档
```

### 1.3 变量与常量

- `var` 声明可变变量。
- `let` 声明不可变常量（绑定不可变，值未必不可变，若为引用类型则可变其内容）。
- 类型标注使用冒号，支持类型推断。

```swift
var name: String = "SukiCode"
let version = 1.0          // 推断为 Double
let count: Int             // 允许延迟初始化，但使用前必须赋值
```

对于 `let` 常量，如果其类型是拥有 `mutating` 方法的结构体，这些方法不能被调用，以保证值不可变。

**延迟初始化规则**：

- `let` 声明可以不立即赋值，但必须在 **所有控制流路径** 上恰好赋值一次。
- 编译器对 `let` 做 **确定性赋值分析**：
  - 不允许在任何路径上读取未赋值的 `let`。
  - 不允许对同一个 `let` 赋值两次。
  - 不允许在条件分支中只赋值部分路径。
- `let` 不能被闭包捕获，除非它在捕获点之前已经确定赋值。
- 在 `init` 中，`let` 存储属性必须在 `self` 可用之前赋值；`convenience init` 必须委托给指定初始化器完成赋值。

### 1.4 基础数据类型

| 类型     | 说明                                   |
|----------|----------------------------------------|
| `Bool`   | true / false                           |
| `Int`    | 平台相关字长有符号整数（32或64位）      |
| `UInt`   | 平台相关无符号整数                     |
| `Int8`…`Int64`, `UInt8`…`UInt64` | 固定位宽整数          |
| `Float`  | 32 位 IEEE 754 浮点数                  |
| `Double` | 64 位浮点数                            |
| `Char`   | **Unicode 标量**（21位值），表示单个 Unicode 码点 |
| `Void`   | 表示无值，用于函数返回类型（可省略）   |
| `Never`  | 无值类型，表示永不返回                 |

### 1.5 字符串与字符

```swift
let hello = "Hello, world"
let multiline = """
   多行字符串
   保留缩进
"""
let interpolated = "版本 \(version)"
let char: Char = "A"
// 原始字符串：r"无转义\\n直接输出"
```

`String` 是值类型，但内部存储采用写时复制（COW）。`Char` 类型代表一个 **Unicode 标量**（不同于 Swift 的 Extended Grapheme Cluster）。`String` 提供了视图用于处理不同粒度的 Unicode：

- `.unicodeScalars`: `Collection<UnicodeScalar>` — Unicode 标量序列
- `.chars`: `Collection<Char>` — 与 `.unicodeScalars` 同义，`Char` 即 Unicode 标量的类型别名
- `.utf8`: `Collection<UInt8>`
- `.utf16`: `Collection<UInt16>`

**说明**：

- SukiCode **不提供** Swift 风格的 Extended Grapheme Cluster 视图。
- 如果需要字形簇边界，必须使用 `I18n` 模块中的 `GraphemeBreaker` 工具，显式调用。
- `.characters` 这个名字 **不再使用**，避免与 Swift 的 `Character` 混淆。

### 1.6 集合类型

- `Array<T>`：有序集合，字面量 `[1, 2, 3]`
- `Dictionary<K,V>`：哈希表，字面量 `["key": "value"]`
- `Set<T>`：无序唯一集合，字面量 `{1, 2, 3}`（需 `T` 遵守 `Hashable`）

所有集合均为值类型，采用写时复制优化。

### 1.7 控制流

```swift
// if-else
if x > 0 {
    // ...
} else if x < 0 {
    // ...
} else {
    // ...
}

// guard 提前退出
guard let value = optionalValue else {
    return
}

// switch (无需 break，支持模式匹配)
switch value {
case 0:
    print("零")
case 1..<10:
    print("个位数")
case let v where v > 100:
    print("大于100: \(v)")
default:
    print("其他")
}

// for-in 循环
for item in array { }
for (key, value) in dictionary { }

// while / repeat-while
while condition { }
repeat { } while condition

// loop：无限循环，只能由 break / return / 抛出错误退出
loop {
    // ...
}
```

`switch` 必须穷举所有可能的值。对于枚举，编译器会进行检查；对于其他类型，必须包含 `default` 分支。`fallthrough` 关键字被保留，用于显式落入下一个 `case`。

**`loop` 规则**：

- `loop` 是无限循环，条件恒为真。
- `loop` 只能通过 `break`、`return`、`throw` 或 `Never` 返回退出。
- `loop` 可以带标签：`outer: loop { ... break outer ... }`。
- `loop` 与 `while true` 语义等价；`loop` 是推荐写法，`while true` 仍允许但 `suki-fmt` 会建议改为 `loop`。
- `loop` 可作为表达式，其值类型为 `Never`（若永不退出）或 `break` 携带的值类型。

### 1.8 区间

- `0..<10` 半开区间
- `1...5` 闭区间
- 支持步进：`stride(from:to:by:)`

### 1.9 代码风格与约定

SukiCode 强制采用以下风格约定，以确保代码整洁、一致。编译器 `sukic` 和格式化工具 `suki-fmt` 会强制执行这些规则。

#### 分号 (`;`)

- **禁止使用分号作为语句结束符**。每个语句独立成行，不使用 `;` 结尾。
- 唯一例外：同一行内写多个语句时，必须用 `;` 分隔，但此写法强烈不推荐。

```swift
// 正确
let a = 10
print(a)

// 错误
let b = 20;

// 不推荐但允许（仅在极简 REPL 场景）
let x = 1; let y = 2
```

#### 缩进与空白

- 使用 **4 个空格** 作为一级缩进，禁止使用制表符。
- 大括号 `{` 不另起新行，紧跟在上文之后，并在 `{` 前加一个空格。
- `else`、`catch` 等关键字不另起行，与前面的 `}` 之间用空格隔开。

```swift
// 正确
if condition {
    doSomething()
} else {
    doOther()
}

// 错误
if condition
{
    doSomething()
}
else {
    doOther()
}
```

- 逗号 `,` 后必须跟一个空格，前无空格：`[1, 2, 3]`。
- 二元运算符前后各留一个空格：`a + b`，但范围运算符 `...` 和 `..<` 前后不加空格：`1...5`。
- 冒号 `:` 在类型标注时，前无空格，后有一个空格：`name: String`。在三目运算符 `? :` 中，`?` 和 `:` 前后各留一个空格。

#### 命名约定

- **类型名**（类、结构体、枚举、协议）：**大驼峰**（UpperCamelCase），如 `MyClass`, `HTTPConnection`。
- **变量、常量、函数、方法、属性、参数标签**：**小驼峰**（lowerCamelCase），如 `myVariable`, `calculateTotal()`。
- **枚举值**：**小驼峰**，如 `case success`, `case invalidInput`。
- **全局常量**：**小驼峰**，无特殊前缀。
- **私有成员**：无强制前缀，按访问控制区分。
- **协议名称**：描述能力时使用形容词（如 `Equatable`），描述角色时使用名词（如 `DataSource`）。

```swift
struct UserProfile { }
let maxRetryCount = 3
func fetchData(from url: String) { }
```

#### 括号与换行

- 函数调用：左括号 `(` 前不加空格，右括号后不加空格（除非后跟语法元素）。
- 控制流语句（`if`, `for`, `while`, `switch` 等）后的圆括号与关键字之间留一个空格。

```swift
// 正确
if (x > 0) {   // 圆括号可选，但建议与条件表达式一起使用
    print(x)
}
for i in 0..<10 { }

// 错误
if(x > 0) { }
```

- 单行表达式块可以省略大括号，但必须写在同一行。

```swift
if error { return }
```

- 函数体、类体等即使只有一行，也建议使用大括号换行书写。

#### 文件组织

- 一个源文件主要定义一个类型（类/结构体/枚举），类型名与文件名一致（如 `User.suki` 定义 `User` 类型）。
- 扩展（`extension`）可以放在同一文件或独立文件中。
- 导入语句（`import`）放在文件顶部，模块声明之后，注释之前。按标准库、第三方库、内部模块分组，每组内按字母序排列。

#### 文档注释

- 公开 API 必须使用 `///` 编写文档注释，支持 Markdown 格式。
- 注释内容应描述作用、参数、返回值和可能抛出的错误。

```swift
/// 计算两个整数的和。
/// - Parameters:
///   - a: 第一个加数。
///   - b: 第二个加数。
/// - Returns: 两个数的和。
func add(_ a: Int, _ b: Int) -> Int {
    return a + b
}
```

#### 禁止的特性

- 禁止使用隐式解包可选类型（`Type!`），除非在极少数与 Objective-C 交互的桥接代码中。
- 禁止使用 `++` 和 `--` 运算符（已移除）。
- 禁止在条件判断中直接使用非布尔值（如 `if x { }` 非法，必须写 `if x != 0 { }`）。

---

## 2. 类型系统

### 2.1 值类型与引用类型

- **值类型**：`struct`, `enum`，赋值时具有 **值语义**——语义上产生独立副本，修改一方不影响另一方。物理实现可采用写时复制（COW）优化，但对外行为始终是值语义。
- **引用类型**：`class`, `actor`，赋值时共享同一实例，由 ARC 管理生命周期。

> “深拷贝”仅描述语义，不描述物理实现。`String`、`Array`、`Dictionary`、`Set` 等值类型在赋值时可能共享底层缓冲区，直到任一方发生写入才分离。这一优化不改变值语义的可观察行为。

### 2.2 结构体

```swift
struct Point {
    var x, y: Double

    func distance(to other: Point) -> Double {
        return ((x - other.x).squared() + (y - other.y).squared()).squareRoot()
    }
}
```

结构体不能继承，但可遵守协议，支持扩展。

### 2.3 枚举

支持原始值（Raw Value）与关联值。内存布局默认为 tagged union（标签+值）。可通过 `@enum(C)` 属性强制使用 C 兼容的布局（无标签，仅联合体），用于与 C 互操作。

```swift
enum Result<T, E: Error> {
    case success(T)
    case failure(E)
}

@enum(C)  // 指定使用 C 联合体布局
enum IntOrFloat {
    case intValue(Int)
    case floatValue(Float)
}
```

### 2.4 类

单一继承，支持协议。引用类型，默认 `strong` 属性。

```swift
class Vehicle {
    var speed: Double = 0
    func describe() -> String { "速度 \(speed)" }
}

class Bicycle: Vehicle {
    var hasBasket: Bool
    init(hasBasket: Bool) {
        self.hasBasket = hasBasket
        super.init()
    }
}
```

类可定义析构器 `deinit`，在引用计数归零时调用。

### 2.5 协议

类似 Swift 协议与 TypeScript 接口的混合，可要求属性、方法、下标、初始化器等。

```swift
protocol Identifiable {
    var id: String { get }
    func identify() -> String
}

// 协议可继承
protocol Persistable: Identifiable {
    func save() throws
}
```

### 2.6 扩展

可对任意类型添加方法、计算属性、下标、协议遵守。

```swift
extension Int {
    var doubled: Int { self * 2 }
}
```

### 2.7 可选类型与可选链

`Type?` 表示可选，`nil` 表示无值。

```swift
var middleName: String? = nil
let fullName = "John \(middleName ?? "")"
// 可选链
let uppercase = middleName?.uppercased()
// 强制解包
print(middleName!)
```

可选链的返回值始终是可选类型。如果链中任何一环为 `nil`，则整个表达式的结果为 `nil`。支持可选链的赋值：当左侧为 `nil` 时，赋值被忽略（不产生任何效果）。

```swift
var dict: [String: Int]? = nil
dict?["key"] = 42   // 无效果，不会崩溃
```

### 2.8 Any 与 AnyObject

- `Any` 可容纳任何类型（包括值类型）。
- `AnyObject` 可容纳任意引用类型。

### 2.9 元组

```swift
let pair = (code: 200, message: "OK")
print(pair.code) // 200
```

可用于函数返回多个值。

### 2.10 类型别名

支持泛型参数：

```swift
typealias CompletionHandler = (Result<Data, Error>) -> Void
typealias StringDictionary<T> = Dictionary<String, T>
```

---

## 3. 函数与闭包

### 3.1 函数定义

```swift
func greet(person name: String, from city: String = "Unknown") -> String {
    return "Hello \(name) from \(city)"
}
// 调用
let msg = greet(person: "Alice", from: "Beijing")
```

参数标签明确区分外部名和内部名，默认参数置于末尾。函数参数默认为 `let`（不可变）。如需在函数内部修改参数副本，可以使用 `var` 前缀，但这不影响调用者。要修改调用者的变量，必须使用 `inout` 参数，并通过 `&` 传递。

### 3.2 函数类型

`(Int, Int) -> Bool` 可作为变量类型。

### 3.3 闭包

```swift
let add: (Int, Int) -> Int = { a, b in return a + b }
// 箭头语法（受 TypeScript 启发）
let multiply = (a: Int, b: Int) => a * b
```

闭包可捕获上下文，支持捕获列表 `[weak self]`、`[unowned self]` 以打破循环引用。捕获列表语法：`[weak varName, unowned self]` 等。闭包有两种形式：

- **Swift 风格**：`{ (参数) -> 返回值 in 语句 }`
- **箭头风格**：`(参数) => 表达式`（只能包含一个表达式，其值被隐式返回）

两种闭包都是 **非逃逸** 的，除非显式标记 `@escaping`。

### 3.4 尾随闭包

若函数最后一个参数为闭包，可写在大括号外：

```swift
array.map { item in item * 2 }
```

### 3.5 自动闭包

使用 `@autoclosure` 包装参数，延迟求值。`@autoclosure` 创建的闭包不接受任何参数。

---

## 4. 面向对象与协议

### 4.1 属性

- **存储属性**：`var` / `let`
- **计算属性**：提供 getter 和可选 setter

```swift
struct Circle {
    var radius: Double
    var diameter: Double {
        get { radius * 2 }
        set { radius = newValue / 2 }
    }
}
```

- **属性观察器**：`willSet` / `didSet`

属性观察器在属性被赋值时调用，即使新值和旧值相同。在初始化过程中（包括 `init` 方法中直接赋值）不会触发属性观察器。`deinit` 中不会触发观察器。

### 4.2 下标

```swift
class Matrix {
    private var grid: [Double]
    let rows, cols: Int
    subscript(row: Int, col: Int) -> Double {
        get { grid[row * cols + col] }
        set { grid[row * cols + col] = newValue }
    }
}
```

### 4.3 继承与重写

子类用 `override` 重写方法、属性、下标。`final` 禁止进一步重写。

### 4.4 必要初始化器与便捷初始化器

```swift
class Base {
    required init() { }
    convenience init(value: Int) {
        self.init()
        // ...
    }
}
```

标记 `required` 的初始化器，所有子类都必须显式或隐式实现。`convenience` 初始化器必须调用同一个类中的另一个初始化器（指定或便捷），并且最终必须调用一个指定初始化器。

### 4.5 扩展与协议默认实现

协议方法可提供默认实现，符合类型可自行覆盖。

### 4.6 类型嵌套

允许在类/结构体/枚举内部定义嵌套类型。嵌套类型可以访问其外部类型的所有静态成员和内部类型。访问控制独立。

---

## 5. 泛型与元编程

### 5.1 泛型函数

```swift
func swap<T>(_ a: inout T, _ b: inout T) {
    let temp = a
    a = b
    b = temp
}
```

### 5.2 泛型类型

```swift
class Stack<Element> {
    var items: [Element] = []
    func push(_ item: Element) { items.append(item) }
}
```

### 5.3 协议约束

```swift
func allEqual<T: Equatable>(_ seq: [T], to value: T) -> Bool {
    seq.allSatisfy { $0 == value }
}
```

多重约束：`where T: X & Y`。可以使用 `where` 子句对关联类型和泛型参数施加更复杂的约束。

### 5.4 关联类型

协议中可使用 `associatedtype`。

### 5.5 不透明返回类型

`func create() -> some View` 类似 SwiftUI。

### 5.6 宏（编译期元编程）

SukiCode 支持声明式宏（类似 Swift Macros）。宏用 `macro` 关键字声明，并必须搭配 **`@freestanding`** 或 **`@attached`** 标注之一；规范中不再使用单独的 `@macro` 写法。宏分两类：

- **`@freestanding` 宏**：独立出现在调用位置，不依附于任何声明。再细分为 `expression`（如 `#stringify(x)`、`#log(msg)`）与 `declaration`（在作用域顶层展开为一组声明）两种角色。
- **`@attached` 宏**：依附于某个声明（类型 / 成员 / 扩展），向其「添加」成员或对成员做变换，如 `@attached(member)`、`@attached(accessor)`、`@attached(peer)`。

#### 宏的实现模型

宏的 **实现体使用 SukiCode 编写**，但只能调用编译器注入的 **`Syntax` 元编程 API**（类型 `SyntaxNode` / `SyntaxExpr` / `SyntaxDecl` 等，由 `import SukiSyntax` 提供，仅在宏库作用域内可见）：

```swift
// 表达式宏：接收单个表达式语法节点，返回「(值, 字符串描述)」语法节点
@freestanding(expression)
macro stringify<T>(_ value: T) -> (T, String) {
    // 用 #makeExpr 以字符串插值（quote）构造语法节点，
    // 其中 \(value) 是 unquote：把参数 value（一个 SyntaxExpr 节点）原样嵌入
    return #makeExpr("((\(value)), \"\(value)\")")
}
```

**`#makeExpr` 与 quote / unquote 的形式化**：

- `#makeExpr(s)` 是一个 **宏期原语**，其参数 `s` 必须是一个 **字符串字面量**；编译器把 `s` 当作一段 SukiCode 表达式源代码解析，产生一个 `SyntaxExpr` 节点作为宏的返回值（对 `freestanding(declaration)` 宏则使用 `#makeDecl` 产生 `SyntaxDecl` 列表）。
- **Quote（引用）**：字符串字面量整体是一个「待解析的模板」；除 `\(...)` 之外的内容按字面量作为源代码。
- **Unquote（反引用）**：`\(node)` 中的 `node` **必须** 是宏参数或宏体内构造出的 `SyntaxNode` 值（**不是**运行期值）。在解析前，编译器先用各 `node` 的 **原始源代码文本** 替换 `\(node)`，再把整段字符串一次性解析为单个语法树。
- **卫生性影响**：`#makeExpr` 解析出的树与手写代码地位相同，随后统一进行下节「卫生性（hygiene）」的重命名处理——展开体内新引入的绑定名会被加上 `__suki_macro_<scopeID>_` 前缀，而 unquote 嵌入的节点（来自调用方）保留其原始作用域、不会被重命名，从而避免注入或遮蔽。
- **防注入 / 歧义**：unquote 只接受 `SyntaxNode` 类型、且以「源代码文本替换 + 整段解析」方式构造，因此无法把任意运行期字符串拼进展开结果；解析在最后一次性完成，也不受插值顺序影响，从根本上防止了代码注入与括号/运算符优先级歧义。
- **边界规则**：`#makeExpr(s)` 中的 `\(node)` **只接受 `SyntaxNode`**（`SyntaxExpr` / `SyntaxDecl` / `SyntaxPattern` 等），不接受运行期值。宏体内 **普通字符串插值不可用**；`"\(runtimeValue)"` 在宏期是非法写法。若需要在生成代码中嵌入字面量文本，使用转义 `\\(` 或辅助原语 `#literal("...")`。`#makeDecl` 与 `#makeExpr` 规则相同，只是产生 `SyntaxDecl` 列表。

- 宏体在 **编译期** 由编译器解释执行（**不是**运行时执行）；其输入是 **语法树节点**，输出也是 **语法树节点**（或供 `freestanding(declaration)` 使用的声明节点列表）。
- 宏体 **不能** 包含任意运行时逻辑、循环依赖、对外部状态（文件 / 网络 / 进程 / 内存）的读写；它只能遍历、构造、重写 `Syntax` 节点，以及做字符串拼接（`#unique`、字面量插值）。
- 宏的实现代码必须与宏的使用者在 **同一个模块**，或作为独立的 **宏库**（通过 `sukipm` 发布，导入后使用）。

#### 调试展开

- `sukic -expand-macros <file>`：将宏展开后的 AST / 源码打印到标准输出，便于排查展开结果。
- `sukipm build --verbose-macro`：构建时在日志中输出每个宏的展开前后差异。

#### 实现建议与错误报告要求

- `#makeExpr` / `#makeDecl` 的实现 **应使用解析器而非字符串拼接**：unquote 替换应产生带源位置的语法节点，再拼接为完整模板，最后解析。
- 错误信息必须指向模板中的具体位置，例如 `#makeExpr` 内第 3 个字符处的语法错误，应报告为宏展开点的子位置。
- 卫生性重命名必须在解析后、类型检查前完成；重命名表应保留原始名与源位置，供调试器与 LSP 使用。
- 宏展开的复杂度上限由编译器设定；超出上限时报告 `MacroExpansionTooComplex` 错误。

#### 宏的安全性与卫生性（hygiene）

1. **沙盒**：宏体运行在受限环境中，**禁止文件 I/O、网络访问、进程启动、系统调用、任意内存读写**。任何越界操作在编译期被拒绝并报错。
2. **卫生性（hygiene）**：每个宏调用点都被分配一个 **唯一作用域 ID**（`scopeID`）。宏展开体内部新引入的绑定名，编译器自动重写为 `__suki_macro_<scopeID>_<origName>` 形式，从而：
   - 不会遮蔽或冲突于调用处的同名符号；
   - 不会意外捕获调用处的变量（宏体引用的名字只在宏自身作用域内解析）；
   - 调用处已有的符号 **绝不会被宏重命名**。
3. **显式导出全局名**：若宏确实要引入一个调用方可见的全局唯一符号（例如生成辅助函数），必须使用 `#unique("base")` 表达式，它生成形如 `__suki_unique_<module>_<base>_<counter>` 的标识符，保证模块内唯一且不与其他 `#unique` 冲突。
4. **不透明性**：宏体无法读取调用点的词法环境，只能拿到显式传入的语法节点参数；这从机制上保证「宏不会泄露调用方作用域」。

> 注：`@attached` 宏向类型添加的成员名（如 `init`、`foo`）属于目标类型的作用域，按正常的访问控制（§10.1）与重载规则处理，不受上述卫生性前缀影响。

---

## 6. 内存管理

SukiCode 核心内存模型：**ARC + Pool + Collection + Owned 所有权**。

### 6.1 ARC 自动引用计数

- 所有 `class` 和 `actor` 实例由 ARC 管理。
- 编译器自动插入 `retain` / `release`。
- 循环引用通过 `weak`（自动置 nil）、`unowned`（非可选，不置 nil）打破。
- 闭包捕获 `[weak self]`、`[unowned self]`。捕获列表支持任意表达式：`[weak delegate = self.delegate]`。

### 6.2 池（Pool）

池是批量资源容器，实现 `PoolProtocol`。

```swift
protocol PoolProtocol {
    associatedtype Resource
    func allocate() -> Resource?   // 线程安全要求：由实现者保证
    func releaseAll()              // 线程安全要求：由实现者保证
    var capacity: Int { get }
}
```

内置池：

- `MemoryPool<T>`：从预分配内存块分配对象，池销毁时统一释放，跳过单独 ARC。
- `ThreadPool`：管理工作线程。
- `ChannelPool`：管理 IPC 通道。
- `ConnectionPool`：管理数据库或网络连接。

**线程安全指南**：

- `allocate()` 可能从多个线程并发调用，实现必须使用同步原语（如 `Mutex`）保护内部状态。
- `releaseAll()` 通常由 `Collection` 在析构时调用，但该析构可能发生在任何线程。为了安全，`releaseAll()` 也应具备线程安全性（例如使用相同的锁）。

**依赖关系与循环检测**：

- 池可以显式声明对其他池的依赖：通过 `@depends(on:)` 标注。
- `Collection` 在添加池时，会检查依赖图是否存在循环。若存在循环依赖，编译报错。
- `shutdown()` 按照依赖关系的逆拓扑序释放池（而非简单的添加逆序）。

```swift
let poolA = MemoryPool<Int>(capacity: 100)
let poolB = MemoryPool<String>(capacity: 200)
poolB.addDependency(on: poolA)   // 确保 poolA 在 poolB 之前释放
```

### 6.3 Collection

`Collection` 是池的注册容器，负责统一生命周期和依赖顺序。

```swift
class Collection {
    func add<P: PoolProtocol>(_ pool: P) -> P
    func remove<P: PoolProtocol>(_ pool: P)
    func shutdown()  // 按依赖关系的逆序释放所有池
}
```

`Collection` 可嵌套，形成资源层级。当 `Collection` 实例引用计数归零时，自动调用 `shutdown()`。

### 6.4 所有权类型 `Owned<T>`

对于无需共享的高性能路径，提供唯一所有权类型：

```swift
var buffer = Owned<[UInt8]>(capacity: 1024)
take(ownership: move buffer)  // 移动后 buffer 不可用
```

**限制**：`Owned<T>` 只能用于 **值类型**（`struct`, `enum`, 基本类型）或 `Unmanaged<T>`（裸指针包装）。若 `T` 为 `class`（ARC 管理），禁止使用 `Owned<T>`，因为唯一所有权语义与 ARC 的多引用模型冲突，会导致双重释放或内存泄漏。编译器会在实例化 `Owned<SomeClass>` 时报错。

**与 COW 类型的独占性**：

- 当 `T` 是 COW 值类型时，`Owned<T>` 在构造与每次移动时调用 `.unique()`。
- `.unique()` 在运行期 **总是可以成功**：
  - 若底层缓冲区引用计数为 1，则原地独占，零拷贝。
  - 若引用计数大于 1，则复制一份新缓冲区，旧副本不受影响。
- 因此 `Owned<T>` 的唯一所有权语义始终成立，不存在“运行期无法 unique”的情况。
- **编译器报错**的情况是：`T` 没有实现 `.unique()`（即不是 COW 类型，也没有提供独占化方法），而不是运行期判定失败。
- 性能建议：若 `T` 是 `Array` / `String` / `Dictionary`，在构造 `Owned<T>` 前避免保留其他副本，可让 `.unique()` 走零拷贝路径。

`Owned<T>` 是一种“移动语义”类型，不能被复制，只能被 **移动**。

### 6.5 非托管指针

在 `unsafe` 上下文中允许原始指针操作：

```swift
unsafe {
    let ptr: UnsafeMutablePointer<Int> = allocate(capacity: 1)
    ptr.pointee = 42
    ptr.deallocate()
}
```

支持 `UnsafePointer`, `UnsafeMutablePointer`, `UnsafeBufferPointer` 等，语义类似 Swift。

---

## 7. 并发与异步

### 7.1 线程池与任务提交

```swift
let pool = ThreadPool(workers: 4)
pool.submit {
    // 执行的代码
}
let future = pool.submitWithResult { () -> Int in
    return compute()
}
let result = future.await()
```

`ThreadPool` 默认采用 **工作窃取** 调度算法。`submitWithResult` 返回的 `Future<T>` 是热启动的。

### 7.2 异步/等待

```swift
async func fetchData() throws -> Data {
    let data = await httpClient.get(url)
    return data
}
```

`async` 函数由 **全局协作线程池** 执行（除非指定自定义执行器）。编译器将 `async` 函数转换为状态机。`throws` 和 `async` 可以组合使用：`async throws`。

**自定义执行器**：

- 可以通过 `@executor` 属性指定函数或类型使用的执行器。
- `@MainActor` 用于要求在主线程上执行的代码。

```swift
@MainActor
func updateUI() { ... }

@executor(MyCustomExecutor.shared)
async func customTask() { ... }
```

### 7.3 结构化并发

- `TaskGroup`：并行创建多个子任务。

```swift
await withTaskGroup(of: Data.self) { group in
    for url in urls {
        group.addTask { await download(url) }
    }
    for await data in group { process(data) }
}
```

- `Task`：非结构化任务，可取消。

**取消机制**：

- `Task` 拥有 `cancel()` 方法和 `isCancelled` 属性。
- 取消是协作式的：任务内部的 `await` 点（包括 `Task.sleep`、通道操作、异步函数调用）会检查取消状态，如果已取消则抛出 `CancellationError`。
- 任务可以使用 `withTaskCancellationHandler` 注册清理代码。
- 子任务继承父任务的取消状态。

**`Duration` 与时间字面量**：

- `Task.sleep`、超时与定时器统一接收 **`Duration`** 类型（由 `core` 模块提供的值类型）。其构造使用静态工厂：`Duration.seconds(5)`、`Duration.milliseconds(250)`、`Duration.microseconds(1)`。
- 句法约定：在需要 `Duration` 的实参位置，用 **前导点** 成员写法 `.seconds(5)`（即 `Duration.seconds(5)` 的隐式成员表达式，由编译器在目标类型为 `Duration` 时解析）。
- 规范 **不** 把 `5.seconds` 这种「字面量 + 成员后缀」当作独立字面量语法——它会造成与成员访问的歧义，且无法静态确定底层单位。因此一律写作 `.seconds(5)` / `.milliseconds(200)`。
- 示例：`await Task.sleep(.seconds(5))`、`future.await(timeout: .seconds(5))`。

```swift
let task = Task {
    do {
        let data = try await fetchData()
    } catch is CancellationError {
        print("任务被取消")
    }
}
task.cancel()
```

**`Future.await()` 与 `await` 关键字的区分（重要）**：

- `future.await()` 是 **阻塞方法**：在 **当前线程** 上等待结果就绪，**占用线程、不挂起任务、也不观察 `Task` 取消**。它属于「线程池 Future」（`ThreadPool.submitWithResult` 产生），用于 **非 async 上下文** 或确实要同步等待的场景。
- `await <expr>` 是语言级 **异步挂起点**：**挂起当前 `async` 任务、让出线程、不阻塞**，并在挂起点检查所在 `Task` 的取消状态（已取消则抛 `CancellationError`）。

> 结论：**不要在 `async` 热路径里把 `future.await()` 当挂起原语**——那会卡住协作线程池。async 内等待请用 `await` 化的接口（例如 `await future`）；同步代码里拿结果才用 `future.await()`。

- `future.await(timeout: .seconds(5))`：阻塞等待，最长 `.seconds(5)`；超时抛 `TimeoutError`（这是 **线程阻塞超时**，与任务取消无直接关系）。
- 明确：**`future.await()` 不会响应任务取消**（它不在 async 任务上下文中运行、不检查取消令牌）。async 上下文里需要「可取消地等待 Future」时，应使用 `await future`——它在后台线程阻塞等待、对外以挂起语义暴露，从而能在 `await` 点响应取消并抛 `CancellationError`。

**`await future` 的正式定义**：

- 能被 `await` 直接等待的值必须遵循 **`Awaitable`** 协议：

  ```swift
  protocol Awaitable {
      associatedtype Value
      func waitForValue() async throws -> Value
  }
  ```

- `Future<T>` 遵循 `Awaitable`，其关联类型 `Value == T`。`await future` 是 **语法糖**，脱糖为 `try await future.waitForValue()`。
- **实现与取消语义**：`Future<T>` 在后台线程（由 `ThreadPool` 托管）执行阻塞计算；`waitForValue()` 在调用处 **挂起当前 `async` 任务** 并注册一个 continuation，把「等待」交给后台线程，自己让出线程。当后台计算完成（或失败）时，运行时唤醒 continuation 并恢复任务。
- 在 `await` 挂起点，运行时检查所在 `Task` 的取消状态：若已取消，则 `waitForValue()` 立即抛 `CancellationError` 而不再等待；同时运行时也会向后台计算发出取消请求（尽力而为，计算本身需协作式响应）。这就把「线程池 Future 的阻塞等待」安全地包装成了「语言级可取消挂起」。

**`Future` 的取消协议**：

```swift
protocol Cancellable {
    func cancel()
    var isCancelled: Bool { get }
}
```

- `Future<T>` 遵循 `Awaitable & Cancellable`。
- `waitForValue()` 在 `await` 挂起点检查取消：若所在 `Task` 已取消，立即抛 `CancellationError`，并调用 `Future.cancel()` 通知后台计算。
- 后台计算通过 `Future.isCancelled` 协作式检查取消；`cancel()` 只是发出请求，不强制终止线程。
- `future.await()`（阻塞方法）不响应任务取消；`await future`（挂起语义）响应取消。两者语义严格区分，不得混用。

### 7.4 Actor

隔离可变状态，防止数据竞争。

```swift
actor Counter {
    private var value = 0
    func increment() { value += 1 }
    func getValue() -> Int { value }
}
```

Actor 内部方法为异步调用：`await counter.increment()`。Actor 的所有属性和方法（除了 `nonisolated` 标记的）都在其自己的 **执行器** 上串行执行。

**可重入性**：Actor 允许在 `await` 挂起点处理其他入队任务，这可能导致状态在 `await` 前后发生变化。因此，编程模式要求：**在 `await` 之后重新检查必要的条件**。

```swift
actor Cache {
    private var storage: [String: Data] = [:]

    func get(key: String) async -> Data? {
        if let cached = storage[key] {
            return cached
        }
        // 假设 fetchFromNetwork 是异步的，并且可能在等待期间其他任务修改了 storage
        let data = await fetchFromNetwork(key)
        // 必须重新检查：也许其他任务已经写入了相同 key 的数据
        if storage[key] == nil {
            storage[key] = data
        }
        return storage[key]
    }
}
```

### 7.5 通道与 IPC

`ChannelPool` 管理进程间通信。

```swift
let pool = ChannelPool(scheme: .tcp)
let ch = try await pool.listen(on: 8080)
ch.onMessage { data in
    // 处理
    return responseData
}
// 客户端
let client = try await pool.connect(to: "localhost:8080")
let reply = await client.send(message: "ping")
```

通道支持消息序列化（默认 JSON，可自定义编解码器）。`Channel` 支持有界和无界两种模式：

- **无界通道**：`send` 永不挂起，缓冲区无限增长（可能耗尽内存）。
- **有界通道**：通过 `Channel(capacity: 10, backpressure: .block)` 指定容量和背压策略：
  - `.block`：当缓冲区满时，`send` 挂起直到有空间。
  - `.dropNewest`：丢弃新发送的消息。
  - `.dropOldest`：丢弃队列中最旧的消息。
  - `.throw`：当缓冲区满时，`send` 抛出 `ChannelFullError`。

#### `select` 语句

`select` 语句用于等待多个通道操作中的 **第一个就绪** 的分支。它类似于 Go 语言的 `select`，但语法更接近 Rust 的宏风格。

**语法**：

```swift
select {
case <expr> <- <channel>: statements        // 发送：将 expr 求值后发送到 channel
case let <pattern> <- <channel>: statements // 接收：从 channel 接收并绑定到 pattern
case _ <- <channel>: statements             // 接收：仅等待就绪、丢弃值
default: statements                          // 非阻塞分支（可选）
}
```

**消歧规则**：

- 左侧没有 `let` / `var` / `_` 的标识符模式，一律视为 **发送表达式**。
- 左侧写 `let pattern`、`var pattern` 或 `_`，一律视为 **接收绑定**。
- `case <- channel` 视为 `case _ <- channel` 的简写，仅接收并丢弃。
- `default` 不带 `case`，使 `select` 非阻塞。

**详细规则**：

1. **发送分支**：`case <expr> <- <channel>` 将 **左侧表达式** 求值后 **发送** 到 **右侧的** `channel`。如果通道可写（有缓冲区空间或接收方等待），则执行该分支并发送。左侧必须是可求值的表达式（字面量、变量、调用等），**不是** 绑定模式。
2. **接收分支**：`case let <pattern> <- <channel>` 从 **右侧的** `channel` **接收** 一个值，并绑定到 **左侧的** `pattern`。也可写为 `case _ <- <channel>` 仅等待通道就绪而不绑定值。如果通道非空或有发送方等待，则执行该分支。
3. **`default` 分支**：如果没有任何其他分支可以立即执行（即所有通道操作都会阻塞），则执行 `default` 分支。**`default` 分支使 `select` 变为非阻塞**。`default` 分支是可选的，若未提供且所有分支均阻塞，则 `select` 会挂起等待第一个就绪的分支。
4. **公平性**：当多个分支同时就绪时，`select` **随机选择** 其中一个执行，以避免饥饿。不保证严格轮询。
5. **超时模拟**：可通过结合 `default` 与手动循环实现超时，或使用 `after` 通道（见示例）。

**示例**：

```swift
// 阻塞等待第一个就绪的通道
select {
case let msg <- ch1:
    print("从 ch1 收到: \(msg)")
case 42 <- ch2:
    print("向 ch2 发送 42 成功")
}

// 非阻塞尝试
select {
case let msg <- ch1:
    print(msg)
default:
    print("没有立即可用的操作")
}

// 超时模式（使用 after 通道）
let timeout = Channel<Bool>(capacity: 1)
Task {
    await Task.sleep(.seconds(5))
    await timeout.send(true)
}
select {
case let data <- dataChannel:
    process(data)
case _ <- timeout:
    print("操作超时")
}
```

**注意**：当前版本的 `select` **只能在 `async` 上下文中使用**（因为通道的发送/接收可能挂起当前任务）。`default` 分支本身不挂起，但 **不会放宽这一环境要求**——即使所有分支都非阻塞，`select` 仍须处于 `async` 函数内。

**同步 `select` 的路线图**：

- 当前版本 `select` 只能在 `async` 上下文中使用，即使所有分支都带 `default`。
- 未来版本将提供 `selectSync { ... }`，要求所有分支非阻塞且必须带 `default`，可在普通函数中使用。
- `selectSync` 与 `select` 共享分支语法，但编译器会静态检查所有分支均不挂起。
- 在此之前，普通函数中需要非阻塞通道操作时，请使用 `tryReceive` / `trySend` 等单通道原语。

### 7.6 原子操作与锁

标准库提供：

- `Atomic<T>`（整数/指针特化）
- `Mutex`, `RWLock`, `Semaphore`, `Condition`
- `DispatchQueue`（串行/并行队列）

---

## 8. 系统编程与底层操作

### 8.1 指针与内存操作

参见 6.5。额外提供：

- `MemoryLayout<T>` 查询大小、对齐、步幅
- 未初始化内存：`UnsafeMutableBufferPointer.allocate(count: type:)`

### 8.2 内联汇编

SukiCode 支持 LLVM 风格的内联汇编：

```swift
unsafe {
    asm("mov $0, $1" : "=r"(output) : "r"(input) : "cc")
}
```

语法与 LLVM 内联汇编兼容，使用 `asm` 关键字，必须在 `unsafe` 块内。

**支持的约束**：

- `r`：任意寄存器
- `m`：内存操作数
- `i`：立即数
- `=r`：输出操作数（写）
- `+r`：读写操作数
- `"cc"`：破坏条件码寄存器
- `"memory"`：破坏内存

### 8.3 直接内存映射 I/O

`MMapRegion` 提供文件或匿名内存映射。

### 8.4 动态库加载

`DynamicLibrary` 类型可在运行时加载 `.so`/`.dylib`/`.dll` 并查找符号：

```swift
let lib = try DynamicLibrary.open("libm.so")
let sinFunc = lib.lookup("sin") as (@convention(c) (Double) -> Double)?
```

### 8.5 系统调用封装

标准库 `sys` 模块直接暴露主要操作系统原语：

- 进程管理：`fork`, `exec`, `waitpid`
- 线程：`pthread_create` 等（但推荐高层抽象）
- 文件 I/O：`open`, `read`, `write`, `ioctl`
- 网络：`socket`, `bind`, `listen`, `accept`
- 信号：`signal`, `sigaction`
- 时间：`clock_gettime`, `nanosleep`

`sys` 模块中的函数是直接系统调用的薄封装，不进行额外的错误检查（除了映射 `errno` 到 `SystemError`）。

**错误映射**：系统调用失败时，抛出 `SystemError`，其中包含：

- `code: Int32`（原始 `errno` 值）
- `message: String`（通过 `strerror` 获得的描述）
- 可通过 `static func fromErrno() -> SystemError` 获取当前错误。

```swift
do {
    try sys.open("/nonexistent", flags: .rdonly)
} catch let err as SystemError {
    print("错误码: \(err.code), 描述: \(err.message)")
}
```

### 8.6 unsafe 块的规则

`unsafe` 块用于声明该区域包含编译器无法保证安全的操作。在 `unsafe` 块之外，以下操作被禁止：

- 直接使用 `UnsafePointer`、`UnsafeMutablePointer` 等非托管指针类型（除非是通过 `&` 自动生成的临时指针）。
- 调用任何标记为 `@_unsafe` 的函数。
- 内联汇编（`asm`）。
- 访问 `Unmanaged` 实例的 `takeRetainedValue()` / `takeUnretainedValue()`。

编译器对 `unsafe` 块内的代码不做任何额外的安全检查，开发者必须自行确保内存安全和类型安全。

### 8.7 裸机支持

通过 `--target bare-metal` 编译选项，可禁用标准库，仅使用 `core` 模块。`core` 模块提供：

- 基本类型（`Int`, `UInt`, `Bool`, `Char`, 指针类型）
- `Never`：无值类型，表示永不返回。`core` 模块必须提供 `Never`，供 `@panic_handler`、`_start`、`loop` 表达式等使用。
- `PanicInfo`：panic 处理函数的参数类型，`core` 模块必须提供。
- `core::panic_handler`：用户必须提供一个 **`@panic_handler`** 标注的函数，签名 `func panic(info: PanicInfo) -> Never`。
- `core::alloc::GlobalAlloc`：若使用分配器，用户需实现 **`@global_allocator`** 标注的全局分配器类型（遵守 `GlobalAlloc` 协议）。
- 启动入口：用户必须提供 `_start` 符号，用 **`@no_mangle public func _start() -> Never`** 定义（裸机下不使用 `extern`，因为入口是被引导器直接跳转、而非被外部按 C ABI 调用）。

**`Never` 规则**：

- `Never` 是空枚举，等价于 `enum Never {}`。
- `Never` 是所有类型的子类型（bottom type），可出现在任何需要返回值的上下文中。
- 在裸机下，`Never` 由 `core` 提供；在标准库下，`Never` 由 `Core` 模块提供。

SukiCode 统一采用 **`@` 风格属性**，不使用 Rust 的 `#[...]` 写法。裸机相关属性一览：

| 属性 | 作用 |
|------|------|
| `@no_mangle` | 禁止名称改写，保留给定符号名（用于 `_start`、C 可链接符号） |
| `@panic_handler` | 标记 panic 处理函数（签名 `func panic(info: PanicInfo) -> Never`） |
| `@global_allocator` | 标记全局分配器类型（遵守 `GlobalAlloc` 协议） |

示例最小的裸机程序：

```swift
// 裸机程序，无标准库
@no_mangle
public func _start() -> Never {
    // 直接操作硬件或调用核心功能
    loop {
        // ...
    }
}

@panic_handler
func panic(info: PanicInfo) -> Never {
    loop {
        // ...
    }
}
```

---

## 9. 错误处理

### 9.1 抛出与捕获

函数标记 `throws`：

```swift
func readFile(path: String) throws -> String { ... }
do {
    let content = try readFile(path: "/tmp/data")
} catch let error as FileError {
    print(error)
} catch {
    // 通用错误
}
```

`throws` 函数可以抛出任何遵守 `Error` 协议的类型。`do-catch` 块中的 `catch` 子句使用与 `switch` 相同的模式匹配语法。

### 9.2 可选处理

`try?` 将错误转换为 `nil`，`try!` 断言无错误（失败则崩溃）。

### 9.3 错误类型

任何遵守 `Error` 协议的类或结构体均可作为错误抛出。`Error` 协议是一个空协议，但提供默认的 `localizedDescription` 计算属性（返回 `String(describing: self)`）。支持通过 `as?` 进行类型转换。

```swift
enum FileError: Error {
    case notFound(path: String)
    case permissionDenied
}

extension FileError: CustomStringConvertible {
    var description: String {
        switch self {
        case .notFound(let path): return "File not found: \(path)"
        case .permissionDenied: return "Permission denied"
        }
    }
}
```

### 9.4 结果类型

`Result<T, E: Error>` 泛型枚举，用于异步或不抛出的场景。

---

## 10. 模块、访问控制与编译

### 10.1 访问级别

- `public`：模块外可见
- `internal`（默认）：模块内可见
- `fileprivate`：当前文件可见
- `private`：当前作用域可见

### 10.2 模块定义

每个包（通过 SukiPM 管理）对应一个模块，模块名与项目名称一致，由清单文件 `<ProjectName>.sukiproj` 定义。`import` 引入其他模块。

### 10.3 条件编译

```swift
#if os(Linux)
    // Linux 特定代码
#elseif os(Windows)
    // Windows 特定代码
#else
    // 其他
#endif
```

支持自定义编译标志：`-D FLAG` 或 `-D LEVEL=5`。在代码中可以使用 `#if LEVEL == 5` 进行条件编译。

支持的预定义条件：

- 操作系统：`os(Linux)`, `os(macOS)`, `os(Windows)`, `os(iOS)`, `os(Android)`, `os(FreeBSD)` 等。
- 架构：`arch(x86_64)`, `arch(arm64)`, `arch(arm)`, `arch(riscv64)`。
- 编译器版本：`suki(>=1.0)`。
- 自定义标志：通过 `-D NAME[=VALUE]` 定义。

### 10.4 编译模式

- `debug`：无优化，包含调试信息
- `release`：优化（默认 `-O2`），去除符号
- `size`：优化大小 (`-Os`)
- `bare-metal`：无操作系统

编译输出为目标平台的静态/动态库或可执行文件。

---

## 11. 包管理器：SukiPM

SukiPM 是 SukiCode 的官方包管理器，类似 SwiftPM / NuGet / Cargo。

### 11.1 清单文件 `<ProjectName>.sukiproj`

每个包根目录必须包含一个项目清单文件，命名为 `<项目名>.sukiproj`。该文件使用 SukiCode 语法描述元信息：

```swift
// MyLibrary.sukiproj
package {
    name: "MyLibrary"
    version: "1.2.0"
    description: "一个有用的库"
    authors: ["Alice <alice@example.com>"]
    license: "MIT"
    targets: {
        defaultTarget: "MyLib"
        targets: [
            Target(name: "MyLib", type: .library),
            Target(name: "MyLibTests", type: .test, dependencies: ["MyLib"])
        ]
    }
    dependencies: {
        // 从默认注册表拉取
        "Alamofire": .version("5.0.0"..."5.9.0"),
        // Git 仓库
        "CustomLib": .git(url: "https://github.com/user/custom.git", from: "2.0.0"),
        // 本地路径
        "LocalUtil": .path("../Utility")
    }
    platforms: [.macOS(11), .linux, .windows, .iOS(15)]
}
```

文件名必须与包名一致。清单文件本身就是合法的 SukiCode 文件。依赖项的版本要求支持 `.exact("1.2.3")`, `.range("1.2.3"..."2.0.0")`, `.branch("main")`, `.revision("abc123")`。

### 11.2 依赖解析

采用语义化版本 (`MAJOR.MINOR.PATCH`)，自动解析满足所有约束的最小版本集合。冲突时报错。使用 PubGrub 算法进行依赖解析，提供友好的错误报告。

### 11.3 命令行工具

```bash
# 创建新包（自动生成 <包名>.sukiproj）
sukipm init MyPackage

# 构建（自动拉取依赖）
sukipm build

# 运行测试
sukipm test

# 发布到注册表
sukipm publish

# 添加/移除依赖
sukipm add Alamofire
sukipm remove CustomLib

# 更新依赖
sukipm update

# 生成文档
sukipm docs

# 清理
sukipm clean
```

### 11.4 注册表

默认公共注册表位于 `registry.sukicode.org`，支持用户自建私有注册表（通过配置文件指定 `registries`）。包发布时需通过认证（API Token），自动生成文档和版本索引。

### 11.5 包版本缓存与锁定

生成 `Package.resolved` 锁定文件（JSON 格式），记录精确版本和哈希值，确保可重现构建。

```json
{
  "version": 1,
  "pins": [
    {
      "package": "Alamofire",
      "repositoryURL": "https://github.com/Alamofire/Alamofire",
      "version": "5.6.0",
      "resolvedRevision": "abc123def456",
      "integrity": "sha256-..."
    }
  ]
}
```

### 11.6 二进制依赖与插件

支持分发预编译框架，通过 `binaryTarget` 指定。需要提供不同平台和架构的二进制文件，并指定 SHA256 校验和。

```swift
binaryTarget(
    name: "MyFramework",
    path: "binaries/MyFramework.xcframework.zip",   // 或 URL
    checksum: "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
)
```

支持构建插件（LSP 代码生成等）。插件是独立的可执行文件，通过标准输入/输出或 `libSukiPM` 与包管理器交互。

### 11.7 基于 GitHub 的注册表

SukiPM 天然支持将 GitHub 作为包注册表的存储后端，提供 **集中式注册表索引** 与 **分布式 Git 仓库** 两种模式。

#### 11.7.1 集中式注册表索引模式

默认公共注册表 `registry.sukicode.org` 采用集中式索引模式。注册表服务端维护一个全局包索引，存储包的元信息（包名、版本、源仓库 URL、源码哈希等），但包本身的源代码和二进制制品存储在其指定的 Git 仓库（如 GitHub）中。

发布包时，`sukipm publish` 会验证开发者对目标 GitHub 仓库的写入权限（通过 GitHub OAuth 或 Personal Access Token），计算源码哈希，将版本条目提交到注册表索引，同时将包的源代码推送到指定的 GitHub 仓库并打上版本标签。

#### 11.7.2 分布式 Git 仓库模式（GitHub 作为直接注册表）

对于小型项目、内部私有包或希望完全摆脱中心化注册表的场景，SukiPM 支持 **零基础设施** 的包管理模式：**一切以 Git 仓库为源，直接使用 GitHub 作为注册表**。

在这种模式下，SukiPM 遵循 **约定优于配置** 原则：

- 如果依赖声明为 `"MyPackage"` 而不指定 `url` 字段，SukiPM 会自动推断其源仓库为 `github.com/<namespace>/MyPackage`。
- 开发者只需要将仓库推送到 GitHub 上的约定位置，包即可被发现和使用。
- 每个包仓库的根目录必须包含 `.sukipkg` 元信息文件，声明包的命名空间、名称和版本策略。

```swift
// MyPackage.sukipkg
package {
    namespace: "myorg"
    name: "MyPackage"
    version: "1.0.0"
}
```

> **`.sukiproj` 与 `.sukipkg` 的关系**：`.sukiproj` 是构建系统**唯一消费**的权威项目清单；`.sukipkg` 只是分布式 Git 模式下的轻量**注册描述符**，用于在缺少中心索引时辅助发现与版本策略声明。二者可共存；若并存，**以 `.sukiproj` 为准**。`.sukipkg` 可由 `.sukiproj` 自动生成，不应手工维护两套互相矛盾的元信息。

**版本标签规范**：版本号必须使用语义化版本标签，支持 `v1.0.0` 或 `1.0.0` 两种格式，工具自动识别。推荐使用 `v` 前缀。

#### 11.7.3 私有注册表认证与安全性

私有注册表服务支持多种 GitHub 认证方式：

- **GitHub OAuth App**：适用于公共组织的包管理。
- **Personal Access Token (PAT)**：适用于个人或自动化流水线。
- **GitHub App**：适用于企业级统一管理，可精细控制仓库访问权限。

Token 存储位置：

- macOS：系统密钥链（Keychain）
- Windows：凭据管理器（Credential Manager）
- Linux：加密的本地文件 `~/.config/sukipm/tokens.json`（文件权限 600）

用户应避免将 token 写入项目文件或版本控制系统。SukiPM 提供 `sukipm login` 和 `sukipm logout` 命令安全地管理凭证。

#### 11.7.4 依赖声明语法增强

```swift
// MyApp.sukiproj
dependencies: {
    // 模式一：集中式注册表索引——从默认公共注册表拉取
    "Alamofire": .version("5.0.0"..."5.9.0"),

    // 模式一：私有注册表——明确指定注册表
    "InternalLib": .registry("my-company", .exact("2.1.0")),

    // 模式二：分布式 Git 仓库——直接以 Git 仓库为源
    "MyUtilsGit": .git(url: "https://github.com/myorg/MyUtils.git", from: "1.0.0"),

    // 模式二：约定式推断（自动解析为 github.com/myorg/MyUtilsInfer）
    "MyUtilsInfer": .infer(from: "myorg/MyUtilsInfer", tag: "v1.0.0")
}
```

> 同一依赖键在同一个 `dependencies` 块中只能出现一次；不同模式用不同键名展示。实际项目中按需选择其中一种模式。

---

## 12. 标准库概览

标准库设计为跨平台，提供现代 OS 所需的大部分功能。

### 标准库 1.0 范围

SukiCode 1.0 的标准库分为两层：

**核心必备（1.0 必须交付）**：

- `Core`：基本类型、集合、可选、`Result`、`Range`、打印、断言、数字运算、随机数。
- `System`：进程、环境变量、路径、文件系统、时间、OS 信息。
- `Concurrency`：`ThreadPool`、`Task`、`TaskGroup`、`Actor`、`Future`、`Channel`、`ChannelPool`、`Atomic`、`Mutex`、`Semaphore`。
- `Data`：`Data`、Base64、Hex、GZip、Zlib。
- `Test`：`expect`、`assertEqual`、`assertThrows`、`measure`。

**后续扩展（1.x 逐步交付）**：

- `Network`：`URLSession`、WebSocket、Socket、DNS、TLS、`JSONEncoder/Decoder`、XML、CSV、MessagePack。
- `Crypto`：SHA256、MD5、HMAC、AES、ChaCha20、RSA、Ed25519、证书与密钥管理。
- `I18n`：字符串本地化、数字/日期/货币格式化、Unicode 正规化、字形簇工具。
- `CLI`：`ArgumentParser`、彩色输出、进度条。
- `SukiUI`：可选包，不内置核心标准库。

主要模块详述：

### 12.1 核心 (`Core`)

基本类型（`Int`, `String`, `Array`, `Dictionary`, `Set`, `Optional`, `Result`, `Range`）、打印、断言、数字运算、随机数。`print` 函数是线程安全的，向标准输出写入。

### 12.2 系统 (`System`)

- 进程：`Process`, `Command`（执行外部命令）
- 环境变量、路径操作（`Path` 类型，跨平台路径分隔）。`Path` 提供不可变的路径操作，如 `appending(component:)`、`deletingLastComponent()`。
- 文件系统：`FileManager`, `File`, `FileHandle`（读写）、文件监控
- 时间：`Date`, `TimeInterval`, `Timer`, 日历组件
- 操作系统信息：`OS`, `Hardware`

### 12.3 内存与并发 (`Concurrency`)

- `ThreadPool`, `TaskGroup`, `Task`, `Actor`
- `DispatchQueue`, `Lock`, `Atomic`, `Semaphore`
- `Channel`, `ChannelPool`, `Pipe`
- `Future` / `Promise`

### 12.4 网络 (`Network`)

- `URL`, `URLSession`（HTTP 客户端/服务器）
- WebSocket 客户端/服务器
- TCP/UDP 套接字（低层 `Socket` 类）
- DNS 解析、TLS 集成
- 序列化/反序列化：`JSONEncoder/Decoder`, `XML`, `CSV`, `MessagePack`

### 12.5 加密与安全 (`Crypto`)

- 哈希：`SHA256`, `MD5`, `HMAC`
- 对称加密：`AES`, `ChaCha20`
- 非对称：`RSA`, `Ed25519`
- 安全随机数生成
- 证书与密钥管理

### 12.6 数据与序列化 (`Data`)

- `Data` 字节序列，与指针互操作
- 编解码：Base64, Hex
- 压缩：GZip, Zlib

### 12.7 国际化 (`I18n`)

- 字符串本地化（`.strings` 文件）
- 数字、日期、货币格式化
- Unicode 正规化
- 字形簇工具（`GraphemeBreaker`）

### 12.8 测试 (`Test`)

- XCTest 风格断言：`expect(…)`, `assertEqual(…)`, `assertThrows(…)`
- 异步测试支持：测试函数标记为 `async`，框架自动等待完成。
- 性能基准测试：使用 `measure { ... }` 块，自动运行多次并统计时间。

```swift
test("异步请求") async {
    let result = await fetchData()
    expect(result != nil)
}

test("性能") {
    measure {
        // 被测量的代码
        heavyComputation()
    }
}
```

### 12.9 命令行 (`CLI`)

- 参数解析：`ArgumentParser` 协议，声明式定义命令
- 终端输出：彩色、进度条

### 12.10 图形与 UI（可选）

官方提供的跨平台 UI 库 `SukiUI`，基于声明式语法（类似 SwiftUI），仅作为可选包，不内置核心标准库。但语言本身可通过包引入 GTK、SDL 等绑定。

---

## 13. 与 C / Objective-C 互操作

SukiCode 的 C/ObjC 互操作采用 **声明 + 链接** 模型，不依赖 Clang 集成。用户通过 `extern` 声明告诉编译器外部函数的签名，编译器生成正确的调用代码，最后由链接器连接 C/ObjC 编译好的目标文件。

### 13.1 extern 声明语法

#### 13.1.1 单个 extern 函数

```swift
extern "C" func printf(fmt: UnsafePointer<Int8>, ...) -> Int32
extern "C" func malloc(size: UInt64) -> UnsafeMutablePointer<Void>
extern "C" func free(ptr: UnsafeMutablePointer<Void>)
```

编译器会为这些声明生成 LLVM IR 中的 `declare` 语句，不生成函数体。链接时由系统 C 库提供实现。

#### 13.1.2 extern 块

可以将多个 extern 声明组织在一个块中：

```swift
extern "C" {
    func printf(fmt: UnsafePointer<Int8>, ...) -> Int32
    func scanf(fmt: UnsafePointer<Int8>, ...) -> Int32
    func strlen(s: UnsafePointer<Int8>) -> UInt64
    func memcpy(dest: UnsafeMutablePointer<Void>, src: UnsafePointer<Void>, n: UInt64) -> UnsafeMutablePointer<Void>
}
```

#### 13.1.3 可变参数

使用 `...` 声明可变参数函数：

```swift
extern "C" func printf(fmt: UnsafePointer<Int8>, ...) -> Int32
```

#### 13.1.4 调用约定

默认调用约定为 `"C"`。支持的调用约定：

- `"C"` — C 调用约定（默认）
- `"stdcall"` — Windows stdcall 调用约定

```swift
extern "stdcall" func SomeWindowsAPI(hWnd: UnsafeMutablePointer<Void>, msg: UInt32, wParam: UInt64, lParam: UInt64) -> Int64
```

### 13.2 与 Objective-C 交互

Objective-C 的方法调用本质上是调用运行时库（`libobjc`）的函数。SukiCode 通过 extern 声明 `objc_msgSend` 等运行时函数来实现 ObjC 互操作。

#### 13.2.1 ObjC 运行时函数预声明

编译器自动预声明以下 ObjC 运行时函数，用户无需手动声明：

```swift
typealias Selector = OpaquePointer

// 消息发送
extern "C" func objc_msgSend(self: AnyObject, _cmd: Selector, ...) -> AnyObject
extern "C" func objc_msgSend_stret(self: AnyObject, _cmd: Selector, ...)

// 选择器
extern "C" func sel_registerName(name: UnsafePointer<Int8>) -> Selector

// 类
extern "C" func objc_getClass(name: UnsafePointer<Int8>) -> AnyObject
extern "C" func objc_getProtocol(name: UnsafePointer<Int8>) -> AnyObject

// 引用计数
extern "C" func objc_retain(obj: AnyObject) -> AnyObject
extern "C" func objc_release(obj: AnyObject)
extern "C" func objc_storeStrong(location: UnsafeMutablePointer<AnyObject?>, obj: AnyObject?)

// 弱引用
extern "C" func objc_initWeak(location: UnsafeMutablePointer<AnyObject?>, obj: AnyObject?)
extern "C" func objc_loadWeakRetained(location: UnsafeMutablePointer<AnyObject?>) -> AnyObject?
extern "C" func objc_destroyWeak(location: UnsafeMutablePointer<AnyObject?>)

// 类操作
extern "C" func class_getMethodImplementation(cls: AnyObject, name: Selector) -> OpaquePointer?
extern "C" func class_addMethod(cls: AnyObject, name: Selector, imp: OpaquePointer, types: UnsafePointer<Int8>) -> Bool
extern "C" func objc_allocateClassPair(superclass: AnyObject, name: UnsafePointer<Int8>, extraBytes: UInt64) -> AnyObject?
extern "C" func objc_registerClassPair(cls: AnyObject)
```

#### 13.2.2 调用 ObjC 方法

通过 **类型化函数指针** 调用，避免可变参数 `objc_msgSend` 在 64 位下的 ABI 问题：

```swift
unsafe {
    typealias MsgSend1Ptr = @convention(c) (AnyObject, Selector, UnsafePointer<Int8>) -> AnyObject
    typealias MsgSend0 = @convention(c) (AnyObject, Selector) -> AnyObject

    let nsStringClass = objc_getClass("NSString")
    let sel = sel_registerName("stringWithUTF8String:")

    let send1 = unsafeBitCast(objc_msgSend, to: MsgSend1Ptr.self)
    let str = send1(nsStringClass, sel, "Hello, ObjC!")

    let lengthSel = sel_registerName("length")
    let send0 = unsafeBitCast(objc_msgSend, to: MsgSend0.self)
    let length = send0(str, lengthSel) as! Int
}
```

**字符串桥接规则**：`"Hello, ObjC!"` 是字符串字面量，在参数类型恰好为 `UnsafePointer<Int8>` 时隐式桥接为 NUL 结尾的 UTF-8 指针。运行期 `String` 变量不会自动桥接，须显式 `withCString` 或 `s.cString`。

**补充标准库原语**：

```swift
func unsafeBitCast<T, U>(_ value: T, to type: U.Type) -> U
```

`unsafeBitCast` 必须在 `unsafe` 块中调用，语义与 Swift 相同：按位重新解释类型，不做检查。

#### 13.2.3 ARC 管理

SukiCode 的 ARC 运行时自动管理 ObjC 对象的引用计数。`weak`/`unowned` 同样适用于 ObjC 对象。

#### 13.2.4 #selector 语法

`#selector` 语法用于创建方法选择器：

```swift
let sel = #selector(myMethod(_:))
// 等价于
let sel = sel_registerName("myMethod:")
```

选择器名必须包含参数标签对应的冒号；无参数方法则无冒号。

### 13.3 导出 SukiCode 给 C 使用

通过 `@_cdecl("exported_function")` 导出 **自由函数**（不能是结构体/类的方法），使其可从 C 调用。导出的函数使用 C 调用约定，参数和返回值必须是 C 兼容的类型（基本类型、指针、`OpaquePointer`）。

> **重要**：SukiCode 的 `Int` / `UInt` 是 **平台字长**（64 位平台为 64 位），**不等于** C 的 `int`（通常 32 位）。与 C 互操作必须使用定宽类型 `Int8`/`Int16`/`Int32`/`Int64`、`UInt8`/…/`UInt64`、`Float`、`Double`、指针与 `OpaquePointer`，否则在 64 位平台会出现 ABI 宽度不匹配。

```swift
@_cdecl("add_numbers")
public func addNumbers(a: Int32, b: Int32) -> Int32 {
    return a + b
}
```

在 C 中声明为：

```c
int32_t add_numbers(int32_t a, int32_t b);
```

#### SukiCode ↔ C 类型映射表

| SukiCode 类型 | C 类型 | 备注 |
|---------------|--------|------|
| `Int8` | `int8_t` | 定宽，一一对应 |
| `Int16` | `int16_t` |  |
| `Int32` | `int32_t` | 常用作 C `int` 的精确对应 |
| `Int64` | `int64_t` |  |
| `UInt8` | `uint8_t` |  |
| `UInt16` / `UInt32` / `UInt64` | `uint16_t` / `uint32_t` / `uint64_t` |  |
| `Int` / `UInt` | （无直接对应） | **平台字长**，不要当 C `int` 用 |
| `Float` | `float` | 32 位 IEEE 754 |
| `Double` | `double` | 64 位 IEEE 754 |
| `Bool` | `bool` / `_Bool` | 建议 C 侧用 `bool`（1 字节） |
| `Char` | （无对应） | SukiCode `Char` 是 21 位 Unicode 标量，**不是** C `char`，不可直接互传 |
| `UnsafePointer<T>` / `UnsafeMutablePointer<T>` | `T*` | 指针，需 `unsafe` 上下文 |
| `OpaquePointer` | `void*` | 不透明指针 |
| `Void` | `void` | 仅作返回类型 |
| C 兼容结构体 / `@enum(C)` 枚举 | 对应 C `struct` / 联合体 | 内存布局须一致（见 §2.3、§6.5） |

### 13.4 链接

编译器生成的目标文件（`.o` / `.obj`）包含对外部符号的引用。链接时需要提供包含这些符号的库：

```bash
# 编译 SukiCode 源文件
sukic -c main.suki -o main.o

# 编译 C 源文件
gcc -c mylib.c -o mylib.o

# 链接在一起
gcc main.o mylib.o -o main -lobjc -framework Foundation
```

### 13.5 设计原则

SukiCode 的 C/ObjC 互操作遵循以下设计原则：

1. **不依赖 Clang**：编译器不需要解析 C/ObjC 源代码，只需要知道函数签名。
2. **声明即契约**：用户通过 `extern` 声明告诉编译器外部函数的存在和签名。
3. **链接器负责解析**：编译器生成对外部符号的引用，链接器负责连接实现。
4. **零运行时开销**：extern 函数调用与 C 函数调用完全相同，没有额外的包装层。
5. **类型安全**：extern 声明的参数类型在编译时检查，确保类型安全。

### 13.6 函数值的调用约定：`@convention(c)` / `@convention(stdcall)`

`extern "C" {...}`（§13.1）解决的是 **外部函数声明** 的调用约定；而当需要把「**函数类型的值**」（闭包、函数指针）按某种 C 调用约定传给 C API（如 `qsort` 的比较回调、`lib.lookup("sin")` 取出的函数指针）时，用 `@convention` 属性标注该 **函数类型本身**：

```swift
// 标注该函数类型的值使用 C 调用约定（而非 SukiCode 默认的 Swift 风格快传约定）
let sinFunc = lib.lookup("sin") as (@convention(c) (Double) -> Double)?

// Windows stdcall 回调
typealias WndProc = @convention(stdcall) (IntPtr, UInt32, UInt64, UInt64) -> Int64
```

- `@convention(c)`：值按 C 调用约定传递（平台默认，如 System V AMD64 / cdecl / arm64 AAPCS）。
- `@convention(stdcall)`：值按 Windows `stdcall` 约定传递（x86 上由被调用方清理栈）。
- 带 `@convention` 的函数值可安全传给 `UnsafePointer` 风格的 C 回调参数，或在 `unsafe` 块中通过 `withUnsafeFunctionPointer` 拿到裸函数指针。
- 它与 §13.1 的 `extern "C"` / `extern "stdcall"` 声明层面约定、**§13.3 的 `@_cdecl` 导出** 共同构成完整的 C ABI 映射：声明用 `extern`、导出用 `@_cdecl`、函数类型的值用 `@convention`。

---

## 14. 工具链与项目结构

### 14.1 编译器 `sukic`

```bash
sukic -o output main.suki
sukic build                    # 调用 SukiPM 构建
sukic run                      # 构建并运行
```

`sukic` 是驱动工具，它调用 `suki-frontend`（解析、语义分析）和 `suki-llvm`（LLVM 后端）。支持 `-emit-ir` 输出 LLVM IR，`-emit-ast` 输出语法树。

#### 14.1.1 跨平台编译

`sukic` 基于 LLVM 的模块化架构，本质上是交叉编译器——同一套编译系统支持多种目标平台和架构。通过 `--target` 参数（采用标准的三段式格式 `架构-供应商-系统`）可指定编译目标：

```bash
# 编译到不同平台
sukic --target=x86_64-linux-gnu main.suki          # x86_64 Linux
sukic --target=aarch64-linux-gnu main.suki         # ARM64 Linux
sukic --target=x86_64-windows-gnu main.suki        # x86_64 Windows
sukic --target=aarch64-apple-darwin main.suki      # ARM64 macOS
```

`sukic` 支持的目标平台包括：

| 平台类型 | CPU 架构 | 输出格式 | 示例 target |
|----------|----------|----------|-------------|
| **桌面系统** | x86_64, ARM64, RISC-V | ELF/PE/Mach-O | `x86_64-linux-gnu`, `x86_64-windows-gnu`, `aarch64-apple-darwin` |
| **移动端** | ARMv7, ARM64, x86 | ELF (Android NDK 链接) | `armv7-linux-android`, `aarch64-linux-android` |
| **嵌入式 Linux** | ARM, MIPS, PowerPC, RISC-V | ELF | `arm-linux-gnueabihf`, `riscv64-linux-gnu` |
| **裸机/单片机** | ARM (Cortex-M), RISC-V, AVR | 裸机二进制 | `thumbv7m-none-eabi`, `riscv32imac-unknown-none-elf` |
| **实时操作系统** | ARM, RISC-V | RTOS 专用格式 | `armv7-rtems`, `riscv64-rtems` |
| **Web 浏览器** | WebAssembly | WASM | `wasm32-unknown-unknown` |

编译到不同平台时需要指定目标平台的系统库根路径（`--sysroot`）：

```bash
# 交叉编译到 ARM Linux
sukic --target=arm-linux-gnueabihf --sysroot=/usr/arm-linux-gnueabihf main.suki -o main_arm

# 交叉编译到 Windows
sukic --target=x86_64-windows-gnu --sysroot=/usr/x86_64-w64-mingw32 main.suki -o main.exe

# 交叉编译到 ARM Cortex-M 裸机
sukic --target=thumbv7m-none-eabi --sysroot=./baremetal-sysroot main.suki -o firmware.elf
```

开发者可以查询当前 `sukic` 支持的所有目标架构：

```bash
sukic --list-targets
```

#### 14.1.2 增量编译

`sukic` 支持增量编译，通过 `--incremental` 选项启用。增量编译会缓存每个源文件的中间表示（LLVM IR 和对象文件），仅重新编译发生变化的文件及其依赖。缓存位于 `.build/cache` 目录。

```bash
sukic build --incremental
```

#### 14.1.3 LLVM 自包含后端

`sukic` 采用 **完全自包含的编译后端架构**，所有编译流程（IR 优化、汇编生成、目标代码输出、静态链接）均使用 LLVM/MLIR 基础设施完成，**不依赖任何外部系统编译器**（如 MSVC、GCC、MinGW 等）。

**技术可行性验证**

LLVM 基础设施提供了完整的自包含能力：

1. **集成汇编器 (Integrated Assembler)**：LLVM 的 **MC 层（Machine Code Layer）** 提供了直接生成目标机器码的能力。Clang/LLVM 对于大多数主流目标架构（X86、ARM、AArch64 等）已默认启用集成汇编器（通过 `-integrated-as` 选项），无需调用外部 `as` 命令即可将汇编代码转换为二进制目标码。MC 层通过 `MCAsmBackend`、`MCObjectWriter` 等组件实现二进制编码和对象文件生成，支持 ELF、Mach-O、COFF、WASM 等多种目标文件格式。

2. **内置链接器 LLD**：LLD 是 LLVM 项目的官方链接器，支持 ELF (Linux)、Mach-O (macOS)、COFF (Windows) 和 WASM 等主流格式。LLD 可通过 C++ API 或命令行驱动，性能显著优于传统 GNU ld。

3. **LLVM 后端直接输出对象文件**：LLVM 的目标无关代码生成器框架可以将 LLVM IR 直接转换为指定目标的二进制机器码格式（适用于静态编译器），而不仅仅输出汇编文本。这一能力已在 LLVM 的项目代码中实现（`lib/CodeGen/{ELF,MachO}Writer.cpp`），支持直接输出 `.o` 目标文件。

**实现架构**

`sukic` 将上述组件集成到一个统一的编译驱动中：

```bash
# 完整编译流程——无需外部工具
sukic -c --target=arm-linux-gnueabihf main.suki           # 编译到 .o 对象文件
sukic -o main_arm main.o                                   # 链接生成可执行文件
# 或者一步完成
sukic --target=arm-linux-gnueabihf main.suki -o main_arm
```

**内部流程**：

1. 前端将 `.suki` 源码解析为 AST → LLVM IR（或 MLIR Dialect → LLVM IR）。
2. LLVM 优化器执行平台无关优化 Pass。
3. 调用 LLVM 后端：
   - 如果用户指定 `-S`（输出汇编代码），后端输出汇编文本（适用于需要手动审查的场景）。
   - **默认情况下**，后端通过 **MC 层** 直接将 IR 转换为目标平台的二进制机器码，输出 `.o` 目标文件。
4. 调用 **LLD** 进行静态链接，合并多个 `.o` 文件和系统库，生成最终可执行文件或动态库。
5. 整个流程中，`sukic` 不调用任何外部的 `as`（汇编器）、`ld`（链接器）或 GCC 工具链。

**运行时依赖**：虽然编译器自身不依赖外部汇编器/链接器，但最终生成的可执行文件仍需要目标平台的 **系统库**（如 `libc`、`libc++`）和 C 运行时启动文件（crt1.o 等）。在交叉编译时，用户必须通过 `--sysroot` 提供这些文件。因此，`sukic` 是“自包含的编译器”，不是“完全独立于系统的编译器”。

**高级自定义**

```bash
# 使用集成汇编器（默认开启）
sukic --target=riscv64-linux-gnu --integrated-as main.suki

# 显式禁用集成汇编器，改用外部汇编器（调试用）
sukic --target=arm-linux-gnueabihf --no-integrated-as main.suki

# 指定使用 LLD 链接器（默认）
sukic --target=x86_64-windows-gnu -fuse-ld=lld main.suki

# 输出 LLVM IR 而非直接生成机器码
sukic --emit-llvm main.suki -o main.ll

# 输出目标文件（不链接）
sukic -c main.suki -o main.o
```

### 14.2 语言服务器协议（LSP）

提供 `suki-lsp`，集成 IDE 代码补全、跳转、重构、错误提示。

### 14.3 调试器

生成 DWARF 调试信息，可直接使用 GDB / LLDB 调试。泛型实例化的类型信息也会被包含在 DWARF 中。

### 14.4 代码格式化

`suki-fmt` 自动格式化代码，强制执行 1.9 节中定义的风格规则，并支持通过配置文件 `.suki-fmt.json` 自定义部分选项：

```json
{
    "indentWidth": 4,
    "lineLength": 100,
    "useSpaces": true,
    "trailingCommas": true
}
```

### 14.5 项目布局示例

```
MyApp/
├── MyApp.sukiproj         # 项目清单文件
├── Sources/
│   └── MyApp/
│       └── main.suki
├── Tests/
│   └── MyAppTests/
│       └── tests.suki
├── Resources/
└── .build/                # 构建产物
```

测试文件的命名约定为 `*Tests.suki`。`Resources/` 目录中的文件会被复制到 Bundle 中，可以通过 `Bundle.main.resourceURL` 访问。

### 14.6 文档生成

`sukipm docs` 默认生成 HTML 文档（类似 SwiftDoc），支持 `--format markdown` 输出 Markdown。文档从源代码中的 `///` 注释提取，支持 Markdown 格式和代码高亮。

---

## 15. 设计权衡与哲学

本章节解释 SukiCode 中一些非显而易见的设计选择及其背后的权衡，帮助开发者理解语言的核心理念。

### 15.1 为何同时提供 ARC、Pool、Collection 和 Owned？

- **ARC**：适合大多数常规应用开发，提供自动化、安全性好但略有运行时开销的内存管理。
- **Pool**：适用于高频率、同类型资源的批量管理（如游戏对象、连接池），绕过 ARC 的每个对象计数开销，极大提升性能。
- **Collection**：解决多个池之间的生命周期依赖问题，确保资源按正确顺序释放，避免悬垂引用。
- **Owned**：提供唯一所有权，用于性能敏感或不允许共享的资源（如 DMA 缓冲区）。

这样设计的代价是学习曲线较陡。推荐分层使用：99% 的代码只用 ARC；性能热点改用 Pool；复杂资源关系使用 Collection；底层驱动使用 Owned。

### 15.2 为何 `Owned<T>` 禁止用于 `class`？

因为 `class` 实例由 ARC 管理，本身就允许多个引用。如果强制 `Owned<class>`，会导致两种所有权模型冲突：移动后原变量被禁用，但其他 `strong` 引用仍可能存在，造成内存不安全。因此设计上明确禁止。

### 15.3 为何支持两种闭包语法（Swift 风格和箭头风格）？

- Swift 风格（`{ }`）适合包含多条语句的复杂闭包。
- 箭头风格（`() =>`）受 TypeScript 启发，适合简单表达式，书写更简洁。
两者共存不会引起歧义，因为箭头风格要求 `=>` 左侧是参数列表，语法上可区分。

### 15.4 为何内置 LLVM 自包含后端，而非调用系统编译器？

- **交叉编译简化**：无需在每台机器上安装多套目标平台的 GCC/MinGW，只需一份 LLVM 工具链。
- **性能一致性**：所有目标平台使用相同的优化器，避免不同系统编译器行为差异。
- **增量编译更可靠**：LLVM 的缓存机制可以跨平台复用。

代价是编译器二进制体积较大（~100MB），且升级 LLVM 版本时可能需要全面重新测试。但对于系统语言来说，可控性是值得的。

### 15.5 为何 ObjC 消息语法是可选特性？

为了兼容现有 Objective-C 代码库，但为了避免鼓励非必要的消息发送风格（与现代面向对象语言习惯不同），默认推荐点语法。

**当前状态（重要）**：ObjC 消息语法（方括号 / `obj message:args` 式消息发送）属于 **预留的可选特性**，且 **当前规范尚未正式定义、编译器也未实现**。现阶段与 Objective-C 互操作的唯一途径是通过 §13.2 的 `objc_msgSend` 等运行时函数做 **手动互操作**。未来若正式定义该语法，它将只在显式导入 ObjC 模块后可用，且优先级 **低于** 点语法，以保持代码风格统一。

### 15.6 为何 `String` 的 `Char` 定义为 Unicode 标量而非字形簇？

- **性能**：标量是固定长度（21位），索引操作 O(1)；字形簇需要扫描。
- **C 互操作性**：多数 C 字符串 API 处理的是标量（或 UTF-8 码元）。
- **确定性**：字形簇的边界依赖 Unicode 版本，跨平台行为可能不同。

如果需要处理字形簇，可以使用 `I18n` 模块中的 `GraphemeBreaker` 工具。

### 15.7 为何没有内置的异步 HTTP 客户端？

标准库提供底层 `URLSession` 和 `Socket`，但高级 HTTP 功能放在单独的 `SukiNet` 包中。这样可以保持核心标准库精简，同时允许第三方实现更现代的 HTTP/3 等协议。

### 15.8 为何引入 `unsafe` 块而非依赖模块级标记？

`unsafe` 块提供了细粒度的局部标记，让危险操作的影响范围一目了然，便于代码审查。编译器不会在 `unsafe` 外允许任何不安全操作（如裸指针算术），从而增强整体安全性。

---

*本规范由 林晚晚ss 维护，最后更新于 2026 年。*