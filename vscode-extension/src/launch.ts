import * as vscode from 'vscode';
import * as cp from 'child_process';
import * as fs from 'fs';
import * as net from 'net';
import * as path from 'path';

const children = new Map<string, cp.ChildProcess>();
let terminateHook: vscode.Disposable | undefined;
let output: vscode.OutputChannel | undefined;

const LISTEN_TIMEOUT_MS = 10_000;
const BUNDLED_RUNNER = path.join('bin', 'win32-x64', 'lua-runner.exe');

export function getFreePort(): Promise<number> {
  return new Promise((resolve, reject) => {
    const s = net.createServer();
    s.listen(0, '127.0.0.1', () => {
      const addr = s.address();
      if (addr && typeof addr === 'object') {
        const p = addr.port;
        s.close(() => resolve(p));
      } else {
        reject(new Error('no port'));
      }
    });
    s.on('error', reject);
  });
}

/**
 * Poll until a TCP connect succeeds, then drop the probe socket.
 * Do not use this against lua-runner before VS Code attaches: luadap accepts
 * one DAP client, and a probe close unblocks start(wait=true) so the script
 * runs before the real debugger connects.
 */
export function waitForTcp(
  host: string,
  port: number,
  timeoutMs: number = LISTEN_TIMEOUT_MS
): Promise<void> {
  const started = Date.now();
  return new Promise((resolve, reject) => {
    const attempt = () => {
      if (Date.now() - started > timeoutMs) {
        reject(new Error(`Timed out waiting for DAP on ${host}:${port}`));
        return;
      }
      const socket = net.connect({ host, port }, () => {
        socket.end();
        resolve();
      });
      socket.on('error', () => {
        socket.destroy();
        setTimeout(attempt, 50);
      });
    };
    attempt();
  });
}

function logLine(text: string): void {
  const line = text.replace(/\r?\n$/, '');
  if (!line) {
    return;
  }
  if (!output) {
    output = vscode.window.createOutputChannel('Lua DAP');
  }
  output.appendLine(line);
  vscode.debug.activeDebugConsole.appendLine(line);
}

function pipeChildOutput(child: cp.ChildProcess, onChunk: (s: string) => void): void {
  const handle = (chunk: Buffer | string) => {
    const s = typeof chunk === 'string' ? chunk : chunk.toString('utf8');
    onChunk(s);
    for (const line of s.split(/\r?\n/)) {
      if (line.length > 0) {
        logLine(line);
      }
    }
  };
  child.stdout?.on('data', handle);
  child.stderr?.on('data', handle);
}

function resolveRunnerPath(
  context: vscode.ExtensionContext,
  cfg: vscode.DebugConfiguration
): string {
  const fromCfg = typeof cfg.runnerPath === 'string' ? cfg.runnerPath.trim() : '';
  if (fromCfg) {
    return fromCfg;
  }
  const fromSetting = vscode.workspace
    .getConfiguration('luadap')
    .get<string>('runnerPath', '')
    .trim();
  if (fromSetting) {
    return fromSetting;
  }
  return context.asAbsolutePath(BUNDLED_RUNNER);
}

function ensureTerminateHook(context: vscode.ExtensionContext): void {
  if (terminateHook) {
    return;
  }
  terminateHook = vscode.debug.onDidTerminateDebugSession((session) => {
    killChild(session.id);
  });
  context.subscriptions.push(terminateHook);
}

function killChild(sessionId: string): void {
  const child = children.get(sessionId);
  if (!child) {
    return;
  }
  children.delete(sessionId);
  try {
    if (!child.killed) {
      child.kill();
    }
  } catch {
    /* already gone */
  }
}

function waitUntilListening(
  child: cp.ChildProcess,
  timeoutMs: number
): Promise<void> {
  return new Promise((resolve, reject) => {
    let settled = false;
    let buf = '';
    const timer = setTimeout(() => {
      finish(
        new Error(
          `Timed out waiting for lua-runner DAP listen (${timeoutMs}ms)`
        )
      );
    }, timeoutMs);

    const finish = (err?: Error) => {
      if (settled) {
        return;
      }
      settled = true;
      clearTimeout(timer);
      child.off('error', onError);
      child.off('exit', onExit);
      if (err) {
        reject(err);
      } else {
        resolve();
      }
    };

    const onError = (e: Error) => finish(e);
    const onExit = (code: number | null, signal: NodeJS.Signals | null) => {
      finish(
        new Error(
          `lua-runner exited before DAP listen (code ${code}, signal ${signal})\n${buf}`
        )
      );
    };

    pipeChildOutput(child, (s) => {
      buf += s;
      if (buf.includes('listening on')) {
        finish();
      }
    });
    child.once('error', onError);
    child.once('exit', onExit);
  });
}

async function createLaunchDescriptor(
  context: vscode.ExtensionContext,
  session: vscode.DebugSession
): Promise<vscode.DebugAdapterDescriptor> {
  ensureTerminateHook(context);

  const cfg = session.configuration;
  const programRaw = typeof cfg.program === 'string' ? cfg.program.trim() : '';
  if (!programRaw) {
    throw new Error('Launch configuration requires "program" (Lua file to debug)');
  }

  const cwd =
    (typeof cfg.cwd === 'string' && cfg.cwd.trim()) ||
    vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ||
    process.cwd();
  const program = path.isAbsolute(programRaw)
    ? programRaw
    : path.resolve(cwd, programRaw);
  const args = Array.isArray(cfg.args) ? cfg.args.map(String) : [];
  const host =
    (typeof cfg.host === 'string' && cfg.host.trim()) || '127.0.0.1';
  const port =
    typeof cfg.port === 'number' && cfg.port > 0
      ? cfg.port
      : await getFreePort();

  const runner = resolveRunnerPath(context, cfg);
  if (!fs.existsSync(runner)) {
    throw new Error(
      `lua-runner not found: ${runner}\nBuild target lua-runner and copy it with vscode-extension/scripts/copy-runner.ps1`
    );
  }

  const child = cp.spawn(
    runner,
    ['--host', host, '--port', String(port), '--', program, ...args],
    { cwd, windowsHide: true }
  );
  children.set(session.id, child);
  child.on('exit', () => {
    children.delete(session.id);
    void vscode.debug.stopDebugging(session);
  });

  try {
    await waitUntilListening(child, LISTEN_TIMEOUT_MS);
  } catch (e) {
    killChild(session.id);
    throw e;
  }

  return new vscode.DebugAdapterServer(port, host);
}

export async function createDebugAdapterDescriptor(
  context: vscode.ExtensionContext,
  session: vscode.DebugSession
): Promise<vscode.DebugAdapterDescriptor> {
  const cfg = session.configuration;
  if (cfg.request === 'attach') {
    const host = (cfg.host as string | undefined) || '127.0.0.1';
    const port =
      (cfg.port as number | undefined) ??
      vscode.workspace.getConfiguration('luadap').get('defaultPort', 8172);
    return new vscode.DebugAdapterServer(port as number, host as string);
  }
  if (cfg.request === 'launch') {
    return createLaunchDescriptor(context, session);
  }
  throw new Error(`Unsupported debug request: ${String(cfg.request)}`);
}
