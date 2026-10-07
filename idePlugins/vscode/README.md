# SukiCode VS Code 扩展

为 [SukiCode](../../) 语言（`.suki` 文件）提供编辑器支持的 VS Code 扩展。

## 当前能力

- ✅ **语法高亮**（TextMate 语法 `syntaxes/suki.tmLanguage.json`）
  - 关键字、声明（`func`/`struct`/`enum`/`class`/`protocol`/`actor`/…）
  - 类型名与函数名高亮（含中文标识符）
  - 字符串、三引号字符串、字符串插值 `\()`、字符字面量、转义序列
  - 数值字面量（十进制 / 十六进制 / 二进制 / 浮点）
  - 注释（`//`、`///` 文档注释、`/* */`）
  - 属性 `@main`、`@freestanding(expression)` 等
  - 预处理 / 宏指令 `#if`、`#define`、`#makeExpr(...)` 等
  - 运算符与标点
- ✅ **语言基础配置**（`language-configuration.json`）：注释、括号、自动闭合、折叠
- 🟡 **语言服务器（LSP）**：接口已预留，**默认未启用**；待 `sukic` 提供 LSP 模式后接入。

## 目录结构

```
idePlugins/vscode/
├── package.json              # 扩展清单（贡献语言/语法/命令/配置）
├── language-configuration.json
├── syntaxes/
│   └── suki.tmLanguage.json  # TextMate 语法高亮规则
├── src/
│   ├── extension.ts          # 扩展入口（激活、注册命令）
│   └── lsp/
│       ├── types.ts          # SukiLanguageServer 接口与配置契约
│       └── sukiLsp.ts        # LSP 激活逻辑 + 占位实现（接口预留）
└── tsconfig.json
```

## 构建与调试

```bash
cd idePlugins/vscode
npm install
npm run compile      # 产出 out/extension.js
```

在 VS Code 中按 `F5` 打开“扩展开发宿主”窗口，打开任意 `.suki` 文件即可看到高亮。
重新编译后在新窗口执行“开发者: 重新加载窗口”。

## 接入 LSP（后续步骤）

接口已就绪，待 `sukic` 支持 `sukic lsp`（stdio 传输的 Language Server）后：

1. 将 `vscode-languageclient` 加入 `dependencies`；
2. 在 `src/lsp/sukiLsp.ts` 中取消 `activateSukiLsp` 内“接入真实 LSP 服务器”注释块，
   用 `LanguageClient` 替换 `SukiLspClientStub`；
3. 在 `settings.json` 中设置 `"suki.lsp.enabled": true`。

由于 `SukiLanguageServer` 接口不变，扩展其余代码无需改动即可平滑切换。
