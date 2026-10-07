import * as vscode from 'vscode';

/**
 * SukiCode 语言服务器对外暴露的能力契约。
 *
 * 当前由 {@link SukiLspClientStub} 占位实现；待真实 LSP 服务器就绪后，
 * 由实现同一接口的 `SukiLspClient`（基于 vscode-languageclient）无缝替换，
 * 扩展的其余代码无需改动。
 */
export interface SukiLanguageServer {
  /** 服务器标识，例如 'suki.lsp' 或 'suki.lsp.stub'。 */
  readonly id: string;

  /** 启动服务器（或建立连接）。 */
  start(): Promise<void>;

  /** 停止服务器。 */
  stop(): Promise<void>;

  /** 释放资源。 */
  dispose(): void;

  /** 服务器就绪后的回调（可选）。 */
  onReady?(): void | Promise<void>;
}

/** LSP 相关配置（与 package.json 的 contributes.configuration 对应）。 */
export interface SukiLspConfig {
  /** 是否启用 LSP。默认 false（仅语法高亮）。 */
  enabled: boolean;
  /** LSP 服务器可执行文件路径；留空默认 'sukic lsp'。 */
  serverPath: string;
  /** 服务器启动参数。 */
  serverArgs: string[];
  /** 通信追踪级别。 */
  trace: 'off' | 'messages' | 'verbose';
}
