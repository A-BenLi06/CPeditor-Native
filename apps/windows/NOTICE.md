# Windows 原型第三方声明

Windows 原型通过 NuGet 使用 `WinUIEdit 0.0.5-prerelease`。该二进制包提供 WinUI 3 编辑控件、Scintilla 和 Lexilla。源码研究入口固定为仓库的 `third_party/WinUIEdit` 子模块（提交 `d9f60dbca65c2af54ced590d873d9438de32ce77`）；NuGet 元数据没有给出与二进制包对应的源码提交，因此这里不将二者声明为同一个构建。

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
