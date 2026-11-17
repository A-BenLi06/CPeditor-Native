# macOS 局部性能基准

在仓库根执行：

```sh
./tools/macos/cursor_position_benchmark.sh
```

脚本复用应用的 Xcode／SDK／最低系统版本配置，用 `swiftc -O` 编译独立可执行文件，默认报告写入 `out/macos/cursor-position-perf/benchmark.json`，不构建或启动 GUI、不读取用户文档。可用首个参数指定输出目录。

`CursorPositionBenchmark.swift` 可机器读取的负载参数为 JSON：提供第二参数文件路径即可覆盖 `sizes_bytes`、`samples`、`cached_batch_count`、`build_samples`。默认负载为空、约1／10／16 MiB，分别生成混合 Unicode 多行、单长行、仅 LF 文本；记录字节数／SHA、旧状态栏两次扫描和新模型一次缓存查询的 p50／p95、首次查询、索引构建耗时及逻辑存储开销。查询集中在文件末尾，新路径每个样本平均1024次轻微变化的光标位置，避免单次纳秒计时量化。

这是源码行列查询的优化构建局部测量，不能当作整应用输入／首帧／滚动延迟。索引在首次读取每个已提交版本时仍需扫描一次文本；逻辑存储不包括 Swift 数组容量、分配器、正文、摘要和 GUI。报告保留这些限制，运行结果不作为有噪声的测试阈值。
