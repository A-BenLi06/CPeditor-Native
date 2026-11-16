import AppKit
import SwiftUI

/// 保留并转发 SwiftUI 已安装的窗口代理，额外保护未保存文档。
struct WindowCloseGuard: NSViewRepresentable {
    let workspace: Workspace

    func makeNSView(context: Context) -> GuardView {
        let view = GuardView()
        view.workspace = workspace
        return view
    }
    func updateNSView(_ nsView: GuardView, context: Context) { nsView.workspace = workspace }

    @MainActor final class GuardView: NSView, NSWindowDelegate {
        private struct DelegateReference: @unchecked Sendable {
            let target: any NSWindowDelegate
        }
        weak var workspace: Workspace?
        weak var originalDelegate: (any NSWindowDelegate)?

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            guard let window, window.delegate !== self else { return }
            originalDelegate = window.delegate
            window.delegate = self
            AppTelemetry.lifecycle.info("Workspace window attached")
            if let workspace { ApplicationDelegate.register(workspace) }
        }

        func windowShouldClose(_ sender: NSWindow) -> Bool {
            guard workspace?.canCloseAll() ?? true else { return false }
            return originalDelegate?.windowShouldClose?(sender) ?? true
        }

        override func responds(to aSelector: Selector!) -> Bool {
            if super.responds(to: aSelector) { return true }
            return MainActor.assumeIsolated { originalDelegate?.responds(to: aSelector) ?? false }
        }

        override func forwardingTarget(for aSelector: Selector!) -> Any? {
            // Objective-C 窗口代理分发发生在主线程；这里只将原代理交还给该分发器。
            let reference: DelegateReference? = MainActor.assumeIsolated {
                guard let delegate = originalDelegate, delegate.responds(to: aSelector) else { return nil }
                return DelegateReference(target: delegate)
            }
            if let reference { return reference.target }
            return super.forwardingTarget(for: aSelector)
        }
    }
}
