// swift-tools-version: 6.0
import PackageDescription
import Foundation

let repositoryRoot = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
let coreLibraryDirectory = ProcessInfo.processInfo.environment["CPN_CORE_LIBRARY_DIR"]
    ?? repositoryRoot.appendingPathComponent("out/core/dev").path

let package = Package(
    name: "CPEditorNativeMacOS",
    defaultLocalization: "en",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "CPEditorNative", targets: ["CPEditorNative"]),
        .library(name: "NativeEditorModel", targets: ["NativeEditorModel"])
    ],
    dependencies: [
        .package(url: "https://github.com/mchakravarty/CodeEditorView.git", revision: "2394a48f919d6570cfbfbd53d047a3eaeb95b59e"),
        .package(url: "https://github.com/krzyzanowskim/STTextView.git", revision: "c7d519318b06ebb7a99a641e4aa8831105d3b7d4")
    ],
    targets: [
        .target(name: "NativeEditorModel", resources: [.process("Resources")]),
        .target(name: "CPNativeCore", linkerSettings: [
            .linkedLibrary("cpn_core"), .linkedLibrary("c++"),
            .unsafeFlags(["-L", coreLibraryDirectory])
        ]),
        .target(name: "NativeCoreBridge", dependencies: ["CPNativeCore", "NativeEditorModel"]),
        .executableTarget(name: "CPEditorNative", dependencies: [
            "NativeEditorModel", "NativeCoreBridge",
            .product(name: "CodeEditorView", package: "CodeEditorView"),
            .product(name: "LanguageSupport", package: "CodeEditorView"),
            .product(name: "STTextView", package: "STTextView")
        ], resources: [.process("Resources")]),
        .testTarget(name: "NativeEditorModelTests", dependencies: ["NativeEditorModel"]),
        .testTarget(name: "NativeCoreBridgeTests", dependencies: ["NativeCoreBridge", "NativeEditorModel"]),
        .testTarget(name: "NativeEditorIntegrationTests", dependencies: [
            "CPEditorNative", "NativeEditorModel",
            .product(name: "STTextView", package: "STTextView")
        ])
    ]
)
