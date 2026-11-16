# WinUIEdit 已提交文本快照补丁

此目录保存本项目对固定 WinUIEdit 源码的拟议扩展。上游提交为 `d9f60dbca65c2af54ced590d873d9438de32ce77`。研究子模块保持不变；`tools/windows/prepare-winuiedit.ps1` 将固定提交导出到独立构建副本，校验原文件 SHA256，检查并应用补丁，复制状态机 overlay，再生成完整源码收据。补丁及每个文件的身份以 `source-lock.json` 为准。

## 契约与事务边界

两个原生控件的共同 `IEditorAccess` 接口增加 `IsCompositionActive`、`IsCommittedSnapshotAvailable`、`IsCommittedSnapshotTrackingAvailable`、`CommittedRevision`、`CommittedSnapshotAvailabilityChanged` 和 `ReadCommittedText(out revision, out caretByte, out anchorByte)`。

`ReadCommittedText` 必须在控件所属线程调用。组合输入、未完成的写事务、TSF 锁、待处理消息、待处理通知或队列 drain 期间返回 `E_PENDING`。读取还有自身重入保护：原生字节、revision、光标与选区先同步捕获为不可变值，随后才创建 WinRT Buffer。没有退回读取原始 preedit 缓冲区的路径。

`IsCommittedSnapshotTrackingAvailable` 区分暂时 busy 与永久失效。跟踪故障或 Finalize 后该属性为 false，读取返回明确的 `E_FAIL`，不会返回可无限等待的 `E_PENDING`。应用在发起运行或收到不可用事件时，须检查这一状态；永久故障应报告并取消等待意图，重新创建编辑器后才可恢复跟踪。

在第一段 native write session 或组合输入开始前保存文本基线。TSF TIP 可能先调用 `SetText` 再调用 `OnStartComposition`，因此只从 composition-start 捕获基线不足以判断取消。`OnEndComposition` 只减少活动组合计数；原生锁和队列全部释放后才比较最终文本。净文本改变使 committed revision 增加一次；取消并恢复原文本时不增加。嵌套或重入组合仍使用同一个未结算事务的基线。

失焦、卸载和超时均不被用来推断提交，也不调用强制结束组合输入的方法。Finalize 和不平衡回调使快照不可读取。该设计未改写 Scintilla 的输入法和 undo 逻辑，不能将“快照可读”视为上游撤销正确的证据。

`RequestLock` 的回调与队列异常在 COM 边界转换为 HRESULT；快照基线分配失败在内层处理，只使跟踪失效并继续原生锁和输入。CoreText 锁辅助函数捕获新增的基线分配和队列异常；availability 的发布逻辑为 `noexcept`，连同 Finalize 期间的消费者回调都不会新增异常越界。跟踪失效后不会再重复分配全文基线，也不阻止后续原生编辑；活动 composition 仍按回调计数，committed snapshot 则持续不可读。失效不被伪装为成功，控件需要重新创建才能恢复快照跟踪。

## 构建修正

固定源码的 `packages.config` 与 CppWinRT imports 版本不一致；补丁统一为 `2.0.240405.15`，保留实际使用的 WIL `1.0.260126.7`，声明 C++20，保留 WebView2 `1.0.4022.49` 包与 import，并显式开启 `WebView2UseWinRT`，使 WinUI XAML WinMD 所引用的 `Microsoft.Web.WebView2.Core` 类型能进入 C++/WinRT 投影。构建脚本通过标准 MSBuild 属性选择 WinUI 3，不修改 `UseWinUI3.txt`。

## 已执行的检查

在 macOS ARM64 上，便携状态机经 Clang C++20、严格警告及 ASan/UBSan 检查通过，覆盖候选提交、取消、嵌套、先 SetText 后 composition-start、队列 barrier、不平衡回调、Finalize、包含 NUL 的基线，以及基线分配和消费者回调抛异常的故障注入。故障注入检查异常不越界、快照失效、后续不重复全文分配以及 composition 活动仍被记录。PowerShell 7.7.0 在同一主机实际执行了源码准备、重复验证和篡改拒绝检查：修改的副本会被拒绝且不被覆盖。补丁上下文与所有 after SHA256 均一致。

这些证据仅证明状态模型和准备流程。Windows MSVC、MIDL、WinRT 投影、控件激活、真实 TSF/CoreText 回调、中文输入法、undo、DPI 和性能尚未验证。目标机统一入口见 [Windows 说明](../../apps/windows/README.md)。

## 更新方式

更新上游时先确认新提交的 TSF/CoreText 调用次序和队列行为，再针对新版本维护补丁、overlay 与 before/after SHA256。不要仅修改 source-lock 绕过不匹配。已有构建副本身份变化或文件被修改时，准备脚本要求新输出目录；保留旧副本供追踪。

Windows Server 2022 CI 发现 WinUI XAML 元数据解析需要 WebView2 WinMD。宿主工程声明相同依赖；该 SDK 依赖不改变编辑区使用原生 Scintilla 的路径。此次修正未修改输入状态机或将缺失类型从投影中隐藏。
