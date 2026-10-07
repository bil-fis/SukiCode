import * as vscode from 'vscode';
import type { SukiLanguageServer, SukiLspConfig } from './types';

/** 读取 SukiCode 的 LSP 配置（来自 settings.json 的 suki.lsp.*）。 */
export function getSukiLspConfig(): SukiLspConfig {
  const c = vscode.workspace.getConfiguration('suki');
  return {
    enabled: c.get<boolean>('lsp.enabled', false),
    serverPath: c.get<string>('lsp.serverPath', ''),
    serverArgs: c.get<string[]>('lsp.serverArgs', []),
    trace: c.get<'off' | 'messages' | 'verbose'>('lsp.trace', 'off'),
  };
}

/**
 * LSP 占位实现：在真实语言服务器接入前作为无操作服务器，
 * 提供与真实服务器完全一致的接口，便于后续替换。
 */
class SukiLspClientStub implements SukiLanguageServer {
  readonly id = 'suki.lsp.stub';

  constructor(private readonly output: vscode.OutputChannel) {}

  async start(): Promise<void> {
    this.output.appendLine('[LSP] 尚未接入：语法高亮已可用，语言服务器待实现。');
  }

  async stop(): Promise<void> {
    /* 占位：无需停止 */
  }

  dispose(): void {
    /* 占位：无需释放 */
  }
}

// 当前活动的语言服务器实例（stub 或未来的真实 client）。
let current: SukiLanguageServer | undefined;

/**
 * 在扩展激活时尝试启动 LSP。
 * 默认 suki.lsp.enabled = false，仅准备接口、不建立连接。
 */
export function activateSukiLsp(
  context: vscode.ExtensionContext,
  output: vscode.OutputChannel
): void {
  const config = getSukiLspConfig();

  if (!config.enabled) {
    // 默认不启动 LSP，仅保留接口与配置。语法高亮由 grammar 提供。
    return;
  }

  // ─── 接入真实 LSP 服务器的代码骨架（待实现）──────────────────────────────
  // 当 sukic 提供 Language Server 模式（如 `sukic lsp`）后，按如下方式替换 stub：
  //
  //   import * as path from 'path';
  //   import {
  //     LanguageClient,
  //     TransportKind,
  //     type LanguageClientOptions,
  //     type ServerOptions,
  //   } from 'vscode-languageclient/node';
  //
  //   const serverCommand = config.serverPath || 'sukic';
  //   const serverArgs = ['lsp', ...(config.serverArgs ?? [])];
  //   const serverOptions: ServerOptions = {
  //     run:   { command: serverCommand, args: serverArgs, transport: TransportKind.stdio },
  //     debug: { command: serverCommand, args: [...serverArgs, '--trace'], transport: TransportKind.stdio },
  //   };
  //   const clientOptions: LanguageClientOptions = {
  //     documentSelector: [{ scheme: 'file', language: 'suki' }],
  //     trace: config.trace,
  //   };
  //   const client = new LanguageClient('sukiLsp', 'SukiCode Language Server',
  //                                     serverOptions, clientOptions);
  //   current = client as unknown as SukiLanguageServer;
  //   context.subscriptions.push(client.start());
  //   void client.onReady?.();
  // ───────────────────────────────────────────────────────────────────────────

  current = new SukiLspClientStub(output);
  void current.start();
  context.subscriptions.push({ dispose: () => current?.dispose() });
}

/** 扩展停用时清理 LSP 资源。 */
export function deactivateSukiLsp(): void {
  current?.stop();
  current = undefined;
}
