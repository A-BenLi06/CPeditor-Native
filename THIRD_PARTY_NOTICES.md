# 许可与第三方来源

CPEditor-Native 自有源码按 GPL-3.0-or-later 提供，GPL 第 3 版全文见 LICENSE。带独立许可声明的文件和依赖保留各自条款；最终捆绑分发按解析后的依赖许可图核查。

| 项目 | 固定版本 | 用途与许可 |
| --- | --- | --- |
| CP Editor | 77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c | references 下的研究源码；GPL 第 3 版或更高版本；不进入原生构建 |
| WinUIEdit | d9f60dbca65c2af54ced590d873d9438de32ce77 | Windows 编辑器研究/原型依赖基线；自定义宽松 Scintilla 风格许可，另含 Scintilla/Lexilla 声明 |
| Microsoft.Web.WebView2 SDK | 1.0.4022.49 | Windows WinUI XAML 元数据依赖及 C++/WinRT 投影；适用准确 NuGet 包中的 SDK 许可／第三方声明，不能套用示例仓库的 MIT 许可 |
| nlohmann/json | 55f93686c01528224f448c19128836e7df245f72（v3.12.0） | 共享服务的 JSON 配置与持久化编解码；MIT，原始声明位于 third_party/json/LICENSE.MIT |
| Boost.Asio／Beast | 1.92.0 官方 b2-nodocs 归档；URL／SHA-256 见 third_party/core-dependencies.lock.json | 共享回环 HTTP 分帧与异步 socket；BSL-1.0，原始全文位于 third_party/licenses/boost-1.92.0-LICENSE_1_0.txt；仅内部头文件，不链接动态 Boost runtime |
| CodeEditorView、STTextView | 由 apps/macos/Package.resolved 固定 | macOS 对照原型；分别为 Apache-2.0、GPLv3/商业许可，原型采用 GPL 路径 |

WinUIEdit 的 NuGet 版本和源码基线分别记录在 Windows 项目中；研究源码不是已验证的 Windows 运行产物。SwiftPM 传递依赖的分发许可仍需在发布阶段按实际解析版本核查。

文本位置和 SHA-256 为本项目独立实现，未复制 CP Editor 的 Qt 实现。Unicode 空白规则依据 Unicode White_Space 属性与 Qt QChar 文档/行为核对；此时尚未生成目标安装版本的 Qt 导出兼容测试样本。
