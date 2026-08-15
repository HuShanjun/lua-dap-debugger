import * as vscode from 'vscode';
import { createDebugAdapterDescriptor } from './launch';

export function activate(context: vscode.ExtensionContext) {
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory('lua-dap', {
      createDebugAdapterDescriptor(session) {
        return createDebugAdapterDescriptor(context, session);
      },
    })
  );
}

export function deactivate() {
  // nothing
}
