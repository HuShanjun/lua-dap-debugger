import * as vscode from 'vscode';
import * as cp from 'child_process';

/**
 * LuaDebugSession — 实现 DAP 协议，作为 VS Code 与 Lua 调试后端之间的桥梁
 *
 * 通信方式：通过子进程启动 `lua launch.lua debugger.lua [host] [port] -- program`
 * 前后端通过 TCP socket 直接通信（自定义 JSON 协议）
 * 本类主要负责：解析 DAP 消息、调用后端、发送 DAP 事件/响应
 */
export class LuaDebugSession {
    private emitter: vscode.MessageChannelEmitter;
    private config: any;
    private backendProcess?: cp.ChildProcess;
    private nextCallbackId = 1;
    private pendingRequests = new Map<number, (response: any) => void>();
    private breakpoints = new Map<string, vscode.SourceBreakpoint[]>();
    private initialized = false;

    constructor(emitter: vscode.MessageChannelEmitter, config: any) {
        this.emitter = emitter;
        this.config = config;
        // 监听从后端进程来的数据
        emitter.event((msg) => this.onBackendMessage(msg));
    }

    /**
     * 启动后端 Lua 进程
     * launch: 拉起新进程运行目标脚本
     * attach: 连接已运行的进程（后端已自行连接过来）
     */
    spawnBackend() {
        const request = this.config.request; // "launch" | "attach"

        if (request === "launch") {
            const program = this.config.program;
            const luaexe = this.config.luaexe || "lua";
            const cwd = this.config.cwd || vscode.workspace.workspaceFolders?.[0]?.uri.fsPath || "";
            const args = this.config.args || [];

            if (!program) {
                this.sendError("launch配置缺少 'program' 字段");
                return;
            }

            // 构造命令：lua launch.lua <debugger.lua> [host] [port] -- <program> [args...]
            const backendDir = __dirname; // 这里假设 debugger.lua 在扩展的 backend 目录
            const launchScript = this.resolveBackendPath("launch.lua");
            const debuggerScript = this.resolveBackendPath("debugger.lua");

            const cmdArgs = [
                launchScript,
                debuggerScript,
                "127.0.0.1",
                "8172",
                "--",
                program,
                ...args
            ];

            this.sendOutput(`Starting: ${luaexe} ${cmdArgs.join(" ")}`, vscode.OutputChannelAppearedInDebugConsole);

            this.backendProcess = cp.spawn(luaexe, cmdArgs, {
                cwd: cwd,
                env: { ...process.env }
            });

            this.backendProcess.stdout?.on('data', (d) => this.sendOutput(d.toString(), vscode.OutputChannelShowInDebugConsole));
            this.backendProcess.stderr?.on('data', (d) => this.sendError(d.toString()));
            this.backendProcess.on('exit', (code) => {
                this.sendEvent({ event: 'terminated', body: {} });
            });
        }
        // attach 模式：后端已经连上来了，不需要起新进程
        // 但我们需要一个 placeholder 进程保持会话
        else if (request === "attach") {
            const port = this.config.port || 8172;
            const host = this.config.host || "127.0.0.1";
            // 用一个空进程占位（后端通过 TCP 直接通信，不需要这个进程）
            // 但为了会话管理，我们还是起一个
            this.sendOutput(`Attaching to ${host}:${port}`, vscode.OutputChannelAppearedInDebugConsole);
            // 不发后端进程，因为通信走 TCP
        }
    }

    private resolveBackendPath(filename: string): string {
        // 调试扩展时：指向 src 同级的 lua-runtime
        // 打包时：指向 extension/backend
        const candidates = [
            vscode.Uri.joinPath(vscode.Uri.file(__dirname), "..", "lua-runtime", filename).fsPath,
            vscode.Uri.joinPath(vscode.Uri.file(__dirname), "backend", filename).fsPath,
        ];
        return candidates[0];
    }

    // ===================== DAP 协议处理 =====================

    /**
     * 处理从 VS Code 发来的 DAP 消息
     */
    handleDebugMessage(message: any) {
        switch (message.type) {
            case 'launch':
                this.onLaunch(message);
                break;
            case 'attach':
                this.onAttach(message);
                break;
            case 'setBreakpoints':
                this.onSetBreakpoints(message);
                break;
            case 'configurationDone':
                this.onConfigurationDone();
                break;
            case 'continue':
                this.sendToBackend({ cmd: 'continue', callbackId: this.nextCallbackId++ });
                break;
            case 'next':
                this.sendToBackend({ cmd: 'stepOver', callbackId: this.nextCallbackId++ });
                break;
            case 'stepIn':
                this.sendToBackend({ cmd: 'stepIn', callbackId: this.nextCallbackId++ });
                break;
            case 'stepOut':
                this.sendToBackend({ cmd: 'stepOut', callbackId: this.nextCallbackId++ });
                break;
            case 'disconnect':
                this.sendToBackend({ cmd: 'disconnect', callbackId: this.nextCallbackId++ });
                break;
            case 'evaluate':
                this.sendToBackend({
                    cmd: 'evaluate',
                    callbackId: this.nextCallbackId++,
                    info: { expression: message.expression, context: message.context }
                }, (resp) => {
                    message.respond({ body: { result: resp.result, type: resp.type } });
                });
                break;
            case 'stackTrace':
                this.sendToBackend({
                    cmd: 'stack',
                    callbackId: this.nextCallbackId++,
                    info: {}
                }, (resp) => {
                    const frames = (resp.stack || []).map((f: any, i: number) => ({
                        id: i + 1,
                        name: f.name,
                        location: new vscode.Location(
                            vscode.Uri.file(f.file),
                            new vscode.Position(f.line - 1, 0)
                        )
                    }));
                    message.respond({ body: { stackFrames: frames, totalFrames: frames.length } });
                });
                break;
            case 'scopes':
                this.sendToBackend({
                    cmd: 'scopes',
                    callbackId: this.nextCallbackId++,
                    info: { frameId: message.frameId }
                }, (resp) => {
                    const scopes = (resp.scopes || []).map((s: any, i: number) => ({
                        name: s.name,
                        variablesReference: i + 1,
                    }));
                    message.respond({ body: { scopes } });
                });
                break;
            case 'variables':
                this.sendToBackend({
                    cmd: 'variables',
                    callbackId: this.nextCallbackId++,
                    info: { variablesReference: message.variablesReference }
                }, (resp) => {
                    const vars = (resp.variables || []).map((v: any) => ({
                        name: v.name,
                        value: v.value,
                        type: v.type,
                    }));
                    message.respond({ body: { variables: vars } });
                });
                break;
        }
    }

    private onLaunch(msg: any) {
        this.initialized = true;
        // 通知后端初始化完成
        this.sendToBackend({ event: 'initialized', body: {} });
        msg.response({ body: {} });
    }

    private onAttach(msg: any) {
        this.initialized = true;
        this.sendToBackend({ event: 'initialized', body: {} });
        msg.response({ body: {} });
    }

    private onSetBreakpoints(msg: any) {
        const breakpoints = msg.breakpoints || [];
        const source = msg.source;
        const path = source.path || source.name;

        this.breakpoints.set(path, breakpoints);

        // 发送给后端
        this.sendToBackend({
            cmd: 'setBreakpoint',
            callbackId: this.nextCallbackId++,
            info: {
                source: { path },
                breakpoints: breakpoints.map((b: any) => ({
                    line: b.line,
                    condition: b.condition,
                }))
            }
        }, (resp) => {
            // 返回验证后的断点（命中数等）
            const verified = (resp.breakpoints || []).map((b: any) => ({
                ...b,
                verified: true,
            }));
            msg.response({ body: { breakpoints: verified } });
        });
    }

    private onConfigurationDone() {
        // 所有断点设置完毕，通知后端可以继续
        this.sendToBackend({ event: 'configurationDone', body: {} });
    }

    // ===================== 后端通信 =====================

    private sendToBackend(msg: any, onResponse?: (resp: any) => void) {
        if (onResponse) {
            this.pendingRequests.set(msg.callbackId, onResponse);
        }
        // 通过 TCP 直接发给后端（后端是独立进程，我们这里用进程 stdin/stdout 作为备用通道）
        // 实际实现中，前后端通过 TCP socket 通信
        // 这个 adapter 作为 TCP client 连后端
        this.sendToBackendTCP(msg);
    }

    // 简化：通过进程管道通信（生产环境应改为 TCP）
    private sendToBackendTCP(msg: any) {
        // 在完整实现中，这里应该维护一个 TCP socket 连接
        // 为了演示，我们先通过 process 通信
        if (this.backendProcess && this.backendProcess.stdin) {
            this.backendProcess.stdin.write(JSON.stringify(msg) + '\n');
        }
    }

    /**
     * 处理从后端发来的消息
     */
    private onBackendMessage(msg: any) {
        if (msg.type === 'event') {
            // 后端发来的事件（如 stopped）
            if (msg.event === 'stopped') {
                this.sendEvent({
                    event: 'stopped',
                    body: {
                        reason: msg.reason,
                        threadId: 1,
                        allThreadsStopped: true,
                        text: `${msg.reason} at ${msg.file}:${msg.line}`,
                        source: vscode.Uri.file(msg.file),
                        line: msg.line,
                    }
                });
            } else if (msg.event === 'initialized') {
                // 后端就绪
                this.sendEvent({ event: 'initialized', body: {} });
            }
        } else if (msg.type === 'response') {
            // 后端响应请求
            const handler = this.pendingRequests.get(msg.callbackId);
            if (handler) {
                handler(msg.body || {});
                this.pendingRequests.delete(msg.callbackId);
            }
        }
    }

    // ===================== DAP 事件发送 =====================

    private sendEvent(event: any) {
        this.emitter.send(new vscode.DebugProtocolMessage(event));
    }

    private sendOutput(text: string, visibility: vscode.OutputChannelVisibility) {
        this.emitter.send(new vscode.OutputEvent(`${text}\n`, visibility));
    }

    private sendError(text: string) {
        this.emitter.send(new vscode.OutputEvent(`${text}\n`, vscode.OutputChannelShowInDebugConsole));
    }
}
