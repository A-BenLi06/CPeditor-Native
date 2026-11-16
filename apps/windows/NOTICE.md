# Windows 原型第三方声明

Windows 原型从固定的 `third_party/WinUIEdit` 子模块提交 `d9f60dbca65c2af54ced590d873d9438de32ce77` 导出构建副本，并应用仓库内的 [committed snapshot 补丁](../../patches/winuiedit/README.md)。构建脚本校验源码、补丁与 overlay 的 SHA256；不修改研究子模块工作区。应用以正常 C++/WinRT 项目引用使用该副本构建的 WinMD、DLL 和 PRI，已经移除 `WinUIEdit 0.0.5-prerelease` 二进制包依赖。

本项目的状态机 overlay 与补丁按照本仓库 GPL-3.0-or-later 许可分发；上游 WinUIEdit、Scintilla、Lexilla 的版权与原许可仍须随发行物保留。源码补丁不是上游已发布功能，尚未完成 Windows 编译、输入法和撤销验证。

工程组织参考了 WinUIEdit 的 `CppDemoWinUI3` 示例，未复制其产品图标，也未加入上游清单中的临时工具绕过。原型的图标由 `tools/windows/generate-assets.py` 生成，为本项目的原创几何标记。

WinUIEdit 上游采用以下宽松许可，不能直接标为 MIT。Scintilla 与 Lexilla 的版权及许可须随发行物保留。本项目工程将这些声明及本文件作为部署内容包含在布局中。

## WinUIEdit

```text
License for WinUIEdit

Copyright 2026 by Breece Walker

All Rights Reserved

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted,
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in
supporting documentation.

BREECE WALKER DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS, IN NO EVENT SHALL BREECE WALKER BE LIABLE FOR ANY
SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE
OR PERFORMANCE OF THIS SOFTWARE.
```

## Scintilla

```text
License for Lexilla, Scintilla, and SciTE

Copyright 1998-2021 by Neil Hodgson <neilh@scintilla.org>

All Rights Reserved

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted,
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in
supporting documentation.

NEIL HODGSON DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS, IN NO EVENT SHALL NEIL HODGSON BE LIABLE FOR ANY
SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE
OR PERFORMANCE OF THIS SOFTWARE.
```

## Lexilla

```text
License for Lexilla, Scintilla, and SciTE

Copyright 1998-2021 by Neil Hodgson <neilh@scintilla.org>

All Rights Reserved

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted,
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in
supporting documentation.

NEIL HODGSON DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS, IN NO EVENT SHALL NEIL HODGSON BE LIABLE FOR ANY
SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE
OR PERFORMANCE OF THIS SOFTWARE.
```

## Microsoft 构建及运行依赖

`Microsoft.WindowsAppSDK`、`Microsoft.Windows.CppWinRT` 和 `Microsoft.Windows.SDK.BuildTools` 分别适用其 NuGet 包中的许可证与第三方声明。应用采用 framework-dependent 部署，目标机需要匹配版本和架构的 Windows App SDK 运行时。发布前还需审核实际生成的依赖及许可清单。
