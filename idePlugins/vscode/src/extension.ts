import * as vscode from 'vscode';
import {
  activateSukiLsp,
  deactivateSukiLsp,
  getSukiLspConfig,
} from './lsp/sukiLsp';

export function activate(context: vscode.ExtensionContext): void {
  const output = vscode.window.createOutputChannel('SukiCode');
  context.subscriptions.push(output);
  output.appendLine('SukiCode 扩展已激活：语法高亮已启用，LSP 待接入。');

  // 语法高亮由 package.json 的 contributes.grammars 提供，无需代码干预。
  // LSP 接口已预留（见 src/lsp），默认不启用。
  activateSukiLsp(context, output);

  // 预留命令：重启语言服务器（当前为接口占位）。
  context.subscriptions.push(
    vscode.commands.registerCommand('suki.restartLanguageServer', () => {
      const cfg = getSukiLspConfig();
      if (!cfg.enabled) {
        vscode.window.showInformationMessage(
          'SukiCode LSP 尚未启用（suki.lsp.enabled = false）。语法高亮已可用。'
        );
        return;
      }
      vscode.window.showInformationMessage('SukiCode 语言服务器重启（待接入）。');
    })
  );
}

export function deactivate(): void {
  deactivateSukiLsp();
}
