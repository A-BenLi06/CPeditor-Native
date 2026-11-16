import AppKit
import Foundation

// 仅管理来自传入绝对 bundle 路径的实例，退出请求保留应用的保存保护。
let arguments = CommandLine.arguments
guard arguments.count == 3 else { exit(64) }
let operation = arguments[1]
let url = URL(fileURLWithPath: arguments[2]).standardizedFileURL
func instances() -> [NSRunningApplication] {
    NSWorkspace.shared.runningApplications.filter { $0.bundleURL?.standardizedFileURL == url && !$0.isTerminated }
}

if operation == "quit" {
    let applications = instances()
    applications.forEach { _ = $0.terminate() }
    let deadline = Date().addingTimeInterval(15)
    while applications.contains(where: { !$0.isTerminated }) && Date() < deadline {
        RunLoop.current.run(until: Date().addingTimeInterval(0.1))
    }
    if applications.contains(where: { !$0.isTerminated }) {
        fputs("应用仍在运行；请处理保存提示，或取消本次重新启动。\n", stderr)
        exit(75)
    }
} else if operation == "verify" {
    let deadline = Date().addingTimeInterval(10)
    while instances().isEmpty && Date() < deadline {
        RunLoop.current.run(until: Date().addingTimeInterval(0.1))
    }
    guard let application = instances().first else {
        fputs("没有找到已启动的指定应用 bundle。\n", stderr)
        exit(1)
    }
    // 仅确认 LaunchServices 已登记且进程存活；编辑行为另由测试和 UI 验收验证。
    print("已启动：\(url.path)（PID \(application.processIdentifier)）")
} else { exit(64) }
