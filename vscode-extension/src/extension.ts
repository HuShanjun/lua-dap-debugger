import * as vscode from 'vscode';
import { LuaDebugSession } from './debugger';

// 这个文件只在开发扩展时被加载（F5 调试扩展时）
// 正式使用时，扩展打包后 debug type 直接指向 out/adapter.js

export function activate(context: vscode.ExtensionContext) {
    // 注册 DAP adapter 可执行文件
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterExecutor('lua-dap', new LuaDebugExecutor())
    );
}

export function deactivate() {
    // nothing
}

// DAP Executor — 启动后端进程并管理生命周期
class LuaDebugExecutor implements vscode.DebugAdapterExecutor {
    private session?: LuaDebugSession;

    startSession(_debugSession: vscode.DebugSession, _executable: vscode.DebugAdapterExecutable): vscode.MessageChannelEmitter {
        const emitter = new vscode.MessageChannelEmitter();
        this.session = new LuaDebugSession(emitter, _debugSession.configuration);
        // 启动后端进程
        this.session.spawnBackend();
        return emitter;
    }

    handleExit(_code: number, _signal: string): void {
        this.session = undefined;
    }
}
