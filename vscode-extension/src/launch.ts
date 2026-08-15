import * as vscode from 'vscode';

export async function createDebugAdapterDescriptor(
  _context: vscode.ExtensionContext,
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
  throw new Error('Launch not implemented yet');
}
