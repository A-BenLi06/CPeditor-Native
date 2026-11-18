# 当前 Core 与 C ABI

本文描述已实现接口。完整里程碑仍见 [实施状态](status.md)；支持的调用语义不等于两端目标系统已经验收。

## 调用与线程

`core/include/cpn/api.h` 可由 C、C++ 和 Swift 导入。当前 ABI 为 1，创建时校验 `struct_size` 与 `abi_version`。引擎、任务和缓冲区都是整数句柄；STL、WinRT、AppKit 对象和异常不跨越边界。失败返回 `cpn_status`，可选的调用方 `cpn_error` 保存此调用的说明。

```mermaid
sequenceDiagram
    participant UI as 原生界面
    participant Client as 串行后台协调器
    participant Core as 共享 Core
    UI->>Client: 已提交源码快照与测试
    Client->>Core: submit_json（复制请求）
    Core-->>Client: 任务 ID
    loop 有界排空
        Client->>Core: drain_json
        Core-->>Client: 进度／终态缓冲区
        Client->>Client: 解码并检查文档 ID／revision
        Client->>Core: buffer_release
        Client-->>UI: 更新对应文档结果
    end
    Client->>Core: release_job（终态已排空）
```

各公开操作支持线程安全访问，同一原生协调器应串行排空。`close` 和 `destroy` 会取消并等待固定工作线程，必须在后台调用；销毁前停止调用并协调客户端任务。取消已完成任务幂等，未知或已释放句柄返回错误。缓冲区由库分配，调用方只按长度读取并调用 `cpn_buffer_release`；缓冲区可在引擎销毁后释放，重复释放被拒绝。

## 有界任务与结果

默认 C ABI 配置为 2 个 worker、16 个任务槽、4 个未释放缓冲区、16 MiB 请求和每任务 8 MiB 保留输出。非零选项可覆盖默认值；最大分别为 8、64、16、64 MiB、64 MiB。C++ `JobRegistry` 的独立上限为 32 workers／256 任务，原生产品入口采用 C ABI 的更小上限。

槽覆盖复制中的请求、排队任务、运行任务及完成后尚未 `release_job` 的任务。进度按任务合并为最新一条，每个接受的任务恰好保留一条终态。C++ `poll` 不移除事件；精确序列号 `acknowledge` 不会删除后来产生的进度。C ABI 只在成功分配返回缓冲区后确认其中的事件，容量不足不会丢失终态。

每个 JSON 缓冲区最多 128 MiB，全部存活的 ABI 缓冲区合计最多 256 MiB。排空最多 128 条事件；大批次会收缩为可容纳的事件集合。客户端须释放缓冲区及已消费终态任务，再提交更多工作。结果保留原始 stdout／stderr 字节、实际采集总数和截断标记；文本显示的解码失败不能改变原始结果。

## 请求格式

请求、结果与事件信封的 `schema_version` 当前为 1。请求字段集中在 `core/src/services/json.cpp`，工具链字段集中在 `profile.cpp`。测试 `input` 必须显式提供；缺少或为 null 的 `expected` 表示无答案，空字符串表示答案为空。字节字段接受 UTF-8 字符串或严格 base64 对象。

```json
{
  "schema_version": 1,
  "source": {
    "document_id": "stable-document-id",
    "revision": 42,
    "text": "int main() { return 0; }",
    "original_path": null
  },
  "toolchain": {
    "schema_version": 1,
    "id": "cpp-clang",
    "language_id": "cpp",
    "source_filename": "main.cpp",
    "artifact_name": "solution",
    "build": {
      "executable": "clang++",
      "arguments": ["-std=c++20", "${source}", "-o", "${artifact}"]
    },
    "run": { "executable": "${artifact}", "arguments": [] }
  },
  "tests": [
    { "id": "sample-1", "input": "", "expected": "", "enabled": true }
  ],
  "comparison": "trailing"
}
```

`comparison` 支持 `strict`、`trailing` 和 `byte`；前两者采用已实现的 CP Editor 换行／行尾空白规则。`max_retained_output_bytes` 可降低本次预算，不能突破引擎限额。请求最多 4096 个测试，文档 ID 最多 4096 字节，测试 ID 最多 1024 字节，JSON 嵌套最多 64 层。重复字段／测试 ID、非整数版本或 revision、无效 UTF-8 和 NUL 路径被拒绝。

工具链文档最多 1 MiB，严格拒绝重复／未知字段和数字截断。可选 `build_limits`、`run_limits`、`inherit_environment`、`environment`、`variables` 见 `resources/profiles`。`${source}`、`${artifact}`、`${source_dir}`、`${job_dir}` 及显式变量逐参数展开，执行器直接接收 executable 和 argv，不隐式拼接 shell。

`cpn_validate_toolchain_json` 使用同一解析器验证工具链文档。调用无需引擎句柄，也不启动进程。macOS 在应用或导入工具链配置前调用此接口。

`JobEvent` 状态为 `Queued`、`Building`、`Running`、`Checking`、`Completed`、`Cancelled` 或 `Failed`。终态包含 `result`，其中保留 `document_id`、`revision`、`source_digest`、`profile_id`、编译状态和按稳定测试 ID 标记的结果。解答 AC／WA／CE 仍表示任务正常完成了处理；基础设施错误与取消具有独立分类。UI 必须按源码身份判断结果是否仍适用于当前文档。

## 外部 checker 与共享测试解析

批量请求可增加版本 1 `external_checker` 对象；有答案且解答正常结束时运行独立 checker，其输出纳入同一批次预算。每个结果保留独立 `checker.process`，超时／崩溃等为 `CheckerError`，不改变解答进程结果。配置、退出码协议和限额见 [外部 checker](external-checker.md)。`parse_test_cases` 供 CLI 等入口共享严格测试字段、base64 与数量检查。

## 会话接口

`cpn_engine_session_load` 接收 UTF-8 恢复文件路径，返回含 `status`、`session`、`disk_digest` 与 `detail` 的版本 1 JSON。`cpn_engine_session_save_json` 接收恢复／备份／锁路径、完整会话和上次读取的磁盘摘要，返回文件替换状态。两者使用相同的 ABI 缓冲区释放规则，应在后台协调器中调用；保存受引擎关闭取消信号控制。

保存前完成请求 DOM 清理，并同时预留 64 KiB 返回缓冲区配额和有限响应字符串存储；容量不足不会先写文件再隐藏结果。保存响应按有界字段直接编码，不在实际文件替换之后构造结果 DOM。调用成功的 `CPN_OK` 表示有可解码响应，是否保存成功须继续检查响应 `status` 和 `recovery_replaced`。`DurabilityUnknown` 表示原子替换已经发生但持久化未能确认，不能按普通“未保存”重试。

恢复格式、限额、请求示例和合作锁／外部写入限制见 [会话恢复](session.md)。会话保存请求仍受引擎 `max_request_bytes` 约束。

`cpn_session_default_limits` 返回 Core 的文档、样例、内容和标识符等限额，以及引擎允许的最大请求容量。恢复客户端可用返回的容量配置专用引擎。请求、解码后的会话或磁盘序列化超出容量时，保存接口返回 `CPN_SESSION_CAPACITY` 并保留上次有效记录。

## Companion 收件箱与导入接口

C++ `parse_companion_payload`、`CompanionInboxService` 与 `prepare_import_application` 提供严格解码、持久接纳和文档＋回执的单一候选，进入服务库及单一 Core 归档。C ABI 已增加以下四个导入符号，ABI 仍为 1；均同步调用，原生客户端必须在后台协调器执行。HTTP 生命周期接口见下节；原生导入的验收状态见 [实施状态](status.md)，字段与来源语义见 [Companion 文档](companion.md)。

共同请求是版本 1 对象：

```json
{
  "schema_version": 1,
  "workspace_id": "stable-workspace-id",
  "paths": {
    "inbox_file": "D:\\NativeWorkspace\\Recovery\\inbox.json",
    "backup_file": "D:\\NativeWorkspace\\Recovery\\inbox-backup.json",
    "lock_file": "D:\\NativeWorkspace\\Recovery\\inbox.lock"
  }
}
```

路径须为**当前调用平台**的绝对路径且不含 NUL；不能把 Windows 示例路径传给 macOS。每个接口在这个共同对象中增加相应字段：

| C ABI 符号 | 增加的请求字段 | 响应与语义 |
| --- | --- | --- |
| `cpn_engine_companion_page_json` | 必需 `after_sequence`、`max_items`、`max_page_bytes` | `status`、`detail`、`inbox_id`、`workspace_id`、`revision`、`disk_digest`、`next_after_sequence`、`has_more`、`items`；完整分页包含原始载荷，不消费记录 |
| `cpn_engine_companion_accept_json` | 必需 `original_json` 字符串；可选 `lock_timeout_ms` | 持久接纳／重复／拒绝状态、摘要、两类替换标志及有限 item 摘要；原文不能改成外层 JSON 对象 |
| `cpn_engine_companion_prepare_json` | 必需 `import_id`、完整 `session`；可选 `defaults`、`max_receipts` | 从最新持久 Inbox 取记录，返回 `inbox_status`、准备 `status`、`detail`、摘要、完整候选 `session`、`receipt`；无文件写副作用 |
| `cpn_engine_companion_mark_applied_json` | 必需 `import_id`、`session_paths`；可选 timeout／max_receipts | 通过实际恢复文件回执确认 Applied；写响应格式与 accept 相同，内存候选不能代替实际落盘证据 |

`after_sequence` 为无符号 64 位整数，允许 0；`max_items` 为 1–256，`max_page_bytes` 为 1–128 MiB。所有 envelope、diagnostic、标点与 canonical item 的**实际完整 JSON 字节**都计入页预算。首项或空页 envelope 容不下返回 `CPN_CAPACITY`，不推进／消费；容下前缀则只以最后返回项的 sequence 推进 cursor，并置 `has_more`。Missing 返回空页及 nullable inbox 身份／revision。变化的 Applied 状态不增加 sequence，客户端刷新状态时应从 0 重读，而非仅在旧 cursor 后追加。

`defaults` 接受 `source_text`、nullable `language_id`／`toolchain_profile_id`、`document_preferences`。偏好使用 `{namespace_id,schema_version,object_json}`，在读取 Inbox 前完整验型、预算和递归重复键；不因已有回执走 AlreadyApplied 就忽略坏 defaults。自身 Companion 来源偏好不能预先指定，其他原生偏好原文保留。语言／模板／执行配置由原生配置提供，远端 filename／Java 元数据不能直接成为工具链。

`session_paths` 使用 `{recovery_file,backup_file,lock_file}`。原生流程必须先原子保存 prepare 返回的候选，检查保存响应，再 mark_applied；收到 `AlreadyApplied` 或 `ReceiptMissing` 时遵循持久回执语义，不自动重新创建已删除文档。

可选 `bounds` 对象采用 `CompanionInboxBounds` 同名字段与 `payload` 子对象（`CompanionBounds` 同名字段）。每项只接受正整数，且不超过 C++ 默认值构成的硬上限；`max_id_bytes` 至少 64。`max_receipts` 为 1–256，timeout 为 0–60000 ms（默认 5000）。未知／递归重复字段、浮点版本或限额、负数／溢出和错误路径在副作用前拒绝。

accept／mark 先提取并销毁请求 DOM，同时预留 64 KiB ABI 配额和字符串存储，再调用服务；配额耗尽不会先写盘再隐藏结果。写响应以有限编码生成，不在替换后分配或清理 JSON DOM；只含 item 的稳定身份／状态／摘要／`test_count`／普通 stdio 能力／确认证据，不返回完整载荷或 test ID 数组。诊断中的非法 locale 字节只在诊断显示层替换，载荷原文保持不变。`CPN_OK` 表示得到可解码响应，仍须检查 `status`、`inbox_replaced`、`backup_replaced`；`DurabilityUnknown` 不能按普通无副作用失败重试。

所有响应采用既有 buffer ownership；读／准备在返回前再次检查关闭，写／确认使用引擎 stop token。并发 close 与请求允许 `CPN_CLOSED`／响应 `Cancelled` 的合法竞争，但不能释放尚在使用的对象或吞掉已发生的替换状态。

## Companion 监听生命周期接口

每个引擎最多持有一个 `CompanionListener`。以下三个同步接口同样在原生后台协调器调用，不能在 MainActor／DispatcherQueue 等待线程退出：

| C ABI 符号 | 请求 | 行为 |
| --- | --- | --- |
| `cpn_engine_companion_listener_start_json` | 版本 1 `schema_version`、`workspace_id`、收件箱 `paths`；可选 `bounds`、`options` | 完整验证配置后绑定回环地址；同配置活动实例返回 `AlreadyStarted`，活动实例配置不同返回 `CPN_BUSY`；停止后可创建新实例 |
| `cpn_engine_companion_listener_stop_json` | 仅 `{"schema_version":1}` | 取消并 join 网络／接纳线程；状态为 `Stopped` 或 `NotStarted`，重复停止幂等 |
| `cpn_engine_companion_listener_info_json` | 仅 `{"schema_version":1}` | 返回实例、端口和统计状态；不会启动、停止或消费收件箱 |

`options` 的完整字段及默认值集中映射到 `CompanionListenerOptions`，缺省与显式相同值比较为同配置。降低 `bounds.payload.max_json_bytes` 且省略 `max_body_bytes` 时，默认正文限额同步降低，避免配置自身冲突。

| 字段 | 默认值 | 接口范围 |
| --- | --- | --- |
| `port` / `enable_ipv6` | 10045 / true | 0–65535 / boolean；0 申请临时端口 |
| `max_connections` / `max_admissions` | 16 / 4 | 1–128 / 1–64 |
| `max_body_bytes` | 16 MiB | 1–64 MiB，同时不得超过 payload 限额 |
| `max_inflight_body_bytes` | 32 MiB | 1–128 MiB；正文预留超出合计预算时拒绝请求 |
| `max_header_bytes` / `max_header_count` | 16384 / 64 | 1–65536 / 1–256 |
| `max_response_bytes` | 65536 | 32768–65536 |
| `socket_send_buffer_bytes` | 8192 | 1024–131072 |
| `read_timeout_ms` / `write_timeout_ms` | 5000 / 5000 | 各 1–60000 |
| `request_timeout_ms` | 15000 | 1–300000 |
| `lock_timeout_ms` | 5000 | 0–60000 |

所有整数均严格检查类型、溢出和组合条件；未知／递归重复字段在启动或停止前拒绝。`start` 响应的 `status` 为 `Started`、`AlreadyStarted`、`Closed`、`BindFailed` 或 `Failed`；端口冲突返回 `CPN_OK` 和 `BindFailed`，调用方检查业务状态后可重试。已关闭引擎的启动请求返回 `CPN_CLOSED`，`stop` 和 `info` 仍允许查询／清理。

共同响应包含 `schema_version`、`status`、`has_listener`、`engine_closed`、`port`、`ipv6`、nullable `start_status`、`native_error`、`detail`、`failure_detail` 和 `statistics`。`info.status` 为 `ListenerInfo`。统计包含 `running`／`closed`／`failed` 布尔值、`active_connections`／`outstanding_admissions`／`inflight_body_bytes` 当前用量，以及 `accepted`／`duplicates`／`admission_failures`／`connection_rejections`／`admission_rejections`／`read_timeouts`／`write_timeouts`／`request_timeouts` 计数。没有实例时统计为零；诊断是有界文本，不作状态码解析。

启动／停止前同时预留 64 KiB ABI 配额和有限响应存储；配额不足没有绑定／停止副作用。锁顺序为取得引擎引用后释放全局句柄锁，再进入实例锁，仅在预留／发布缓冲区时短暂访问句柄锁；网络 join 不持有全局句柄锁。`close`／`destroy` 先发出取消，再停止监听器，最后关闭任务注册表；与启动竞争时复查关闭并停止刚启动的实例，不能遗留监听线程。仍存活的响应缓冲区可在销毁引擎后释放。

HTTP 分帧、状态码、持久接纳与有界网络行为见 [Companion HTTP](companion-http.md)。原生界面启停、真实扩展和目标系统验收单独登记，不由 ABI 测试替代。

## 当前限制

- 原生进程后端支持进程组／Windows Job Object、并发三管道、墙钟超时、取消、输出限制和最终排空。未实现内存硬限额；启动期限是软期限，不能中断阻塞的原生进程创建调用。
- 比较循环分批响应取消；内存分配和释放仍同步，不是硬实时保证。
- 每个任务创建独立 workspace，源码快照和产物自动清理。文件写入同目录临时文件后原子替换；POSIX 替换成功后目录持久化失败通过 `AtomicWriteError.destination_replaced` 区分。
- 尚未完成两端 Companion 原生接入的全部验收，以及 LSP、格式化、压力／交互测试和发布接口；这些按完整实施计划继续推进。
