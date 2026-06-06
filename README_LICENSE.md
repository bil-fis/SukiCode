## 许可证 (License)

### 组件许可证一览

| 组件 | 目录 | 许可证 | 说明 |
|------|------|--------|------|
| 编译器核心 | `src/compiler/` | Apache-2.0 WITH Runtime-exception | 词法分析器、解析器、语义分析、代码生成 |
| 运行时库 | `src/runtime/` | Apache-2.0 WITH Runtime-exception | ARC、协程、并发、池 |
| 标准库 | `src/stdlib/` | Apache-2.0 WITH Runtime-exception | Core、System、Network、Crypto、Data、CLI、Test |
| 包管理器 | `tools/sukipm/` | MIT | 依赖管理、构建、发布 |
| 语言服务器 | `tools/suki-lsp/` | MIT | IDE 集成、代码补全、诊断 |
| 格式化工具 | `tools/suki-fmt/` | MIT | 代码格式化 |
| 文档生成器 | `tools/suki-doc/` | MIT | API 文档生成 |
| 语言规范 | `SukiCode_Specification.md` | CC-BY-4.0 | 语言定义文档 |
| 示例代码 | `examples/` | MIT | 示例程序 |
| 测试用例 | `moduleTest/` | MIT | 测试文件 |

### 许可证文件

- `LICENSE_Apache-2.0.txt` — Apache 2.0 许可证全文
- `RUNTIME_EXCEPTION.txt` — 运行时库例外条款
- `LICENSE_MIT.txt` — MIT 许可证全文
- `LICENSE_CC-BY-4.0.txt` — CC BY 4.0 许可证全文

### 核心声明

> **用 SukiCode 编写的程序可以使用任何许可证（包括闭源商业软件）。**
>
> SukiCode 的运行时库例外条款确保：你用 SukiCode 编译器生成的程序不会因为链接了运行时库而被强制开源。

### 常见问题 (FAQ)

**Q: 我用 SukiCode 写商业软件，会被强制开源吗？**

A: 不会。SukiCode 的运行时库例外条款（Runtime Library Exception）明确声明：用 SukiCode 编译器生成的程序（Compiled Programs）不属于编译器或运行时的衍生作品。你可以自由选择任何许可证，包括闭源商业许可证。

**Q: 我想修改编译器本身，需要遵守什么？**

A: 修改后的编译器必须基于 Apache 2.0 + Runtime Exception 开源。具体来说：
- 你对编译器源代码的修改必须以 Apache 2.0 许可证发布
- 运行时库例外条款仍然适用
- 你需要保留原始版权声明和许可证

**Q: 工具（sukipm、suki-lsp、suki-fmt）可以嵌入到我自己的闭源工具中吗？**

A: 可以。这些工具使用 MIT 许可证，允许你在闭源商业软件中使用、修改和分发，只需保留版权声明和许可证文本。

**Q: 我修改了标准库（src/stdlib/），需要开源吗？**

A: 是的。标准库使用 Apache 2.0 + Runtime Exception 许可证。你对标准库的修改需要以相同的许可证发布。但是，你用标准库编写的应用程序代码不受此限制。

**Q: 运行时库例外条款是什么意思？**

A: 运行时库例外条款（类似 LLVM 的 Runtime Library Exception）确保：即使你的程序链接了 SukiCode 的运行时库（如 ARC、协程运行时等），你的程序代码本身不需要遵循 Apache 2.0 许可证。这是为了让你能自由地用 SukiCode 编写闭源软件。

**Q: 我想在自己的项目中使用 SukiCode 的示例代码，可以吗？**

A: 可以。示例代码使用 MIT 许可证，你可以在任何项目中自由使用。

### 许可证兼容性

```
Apache-2.0 WITH Runtime-exception
├── 兼容: MIT, BSD, ISC, Apache-2.0
├── 不兼容: GPL-2.0-only (单向兼容: Apache→GPL3 可以, GPL2→Apache 不行)
└── 说明: 运行时例外消除了 GPL 的"传染性"对编译产物的影响

MIT
├── 兼容: 几乎所有许可证
└── 说明: 最宽松的许可证之一

CC-BY-4.0
├── 适用: 仅文档
└── 要求: 归因、链接到许可证、标注修改
```
