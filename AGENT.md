# tpix 项目说明（供 AI Agent 阅读）

## 1. 项目定位

tpix（仓库名 ScreenCapture）是一个 **Windows 开源截图贴图工具**，对标 PixPin。
技术栈：**C++20 + Win32 + Direct2D + Windows Composition + 自研 GUI 框架 Ling（xland/Ling）**。无 Qt、无 MFC、无第三方 UI 库。

命令行/工程布局：

| 项 | 值 |
| --- | --- |
| 解决方案 | `ScreenCapture.slnx` |
| 工程文件 | `Src/ScreenCapture.vcxproj` |
| 平台 | x64 / Win32，只有 **x64** 的日常配置可用（见下） |
| 工具集 | `v145`（VS 2026）；本机若只有 VS 2022 的 `14.44`，需临时改成 `v143`/`v144` 才能编译 |
| 预编译头 | `Src/pch.h`（`ClCompile` 里 `PrecompiledHeader=Use`，`pch.cpp` 为 `Create`） |
| 字符集 | Unicode；编译选项含 `/utf-8` |

## 2. 外部依赖（**编译前必须就位**）

工程 Sapphire 里有硬编码绝对路径，缺任一项都无法编译：

| 依赖 | 期望路径 | 说明 |
| --- | --- | --- |
| **Ling 框架** | `D:\project\Ling` | Include 目录；链接 `D:\project\Ling\x64\$(Configuration)\Ling.lib`。Ling 自身需要先编译（它是 StaticLibrary，内含 yoga 子工程，产物 `Yoga.lib` + `Ling.lib`） |
| **gifski** | `D:\sdk\gifski` | 链接 `D:\sdk\gifski\target\release\gifski.lib` |

附加库：`dwmapi;windowsapp;comctl32;imm32;version;mf;mfreadwrite;mfplat;mfuuid;ntdll;Userenv;Yoga;Ling`

获取 Ling：`git clone https://github.com/xland/Ling.git`（与 tpix 同一作者）。
**Ling 是自研可控上游**，缺控件能力（字体族、拖放、布局）时直接改 Ling，不存在"等外部排期"。

内联的第三方源码（已在仓库内，无需额外获取）：`Src/quirc`（二维码识别）、`Src/Win/cgif`（GIF 编码）。

## 3. 源码结构

```
Src/
  main.cpp            入口；Ling::init() -> App 事件循环
  App.h/.cpp          进程级能力：虚拟桌面矩形、热键、排除自录(excludeFromCapture)
  Setting.h/.cpp      config.json 读写（%appdata%\ScreenCapture 或 exe 同目录便携版）
  Util.h/.cpp         图像输出：剪切板(CF_DIBV5+CF_DIB+PNG)、存盘、抓屏、二维码、OCR插件
  Lang.h/.cpp         语言包 Lang/*.json（**UTF-16 LE 带 BOM**，结构为两级 map）
  History.h/.cpp      **只负责标注层的 undo/redo**，与"历史截图"无关（命名易误读，勿混淆）
  CutMask / CutMask   拖框、八向手柄、窗口枚举吸附
  Win/
    WinCap           截图主窗口：Select -> Adjust -> Long/Video 三阶段
    WinPin           贴图（钉图）窗口：底图 + shapes + 缩放
    CapLong          滚动截图（自动滚动 + 条带比对拼接，**目前只支持纵向**）
    CapVideo         录屏：VideoMp4.hpp(DXGI+MF) / VideoGif.hpp(BitBlt+cgif)
    WinSetting*      设置窗口：Common / Shortcut / About
  Tool/
    ToolCap          框选完成后的一级工具条（截图阶段）
    ToolMain         贴图窗口的二级工具条（8 个标注工具）
    ToolSub          三级样式条（填充/粗斜体/线宽滑块/色板）
    ToolLong/ToolVideo
  Shape/             8 个标注图形类，基类 ShapeBase
  Tip.h/.cpp         悬停提示小窗口
```

## 4. 关键架构约定（改动前必须理解）

1. **Shape 层依赖 WinPin**：`ShapeBase::win` + `WinPin::history / toolSub / screenImg / scale`。标注能力目前是贴图窗口的私有能力，还不是可挂在任意画布上的通用图层。
2. **两套坐标系**：shape 存的是**底图像素**；贴图窗口的尺寸受 `Ctrl+滚轮` 缩放影响。窗口客户区坐标 → 底图坐标走 `WinPin::toImgPos`。导出走 `getImagePixels()` 离屏重绘，导出图恒为原始像素尺寸。
3. **`Ling::Color(uint32_t)` 是 `0xRRGGBBAA`**（alpha 在最低字节），不是 GDI 的 `0x00BBGGRR`。GDI `COLORREF` 转进来必须手工重排通道。
4. **Ling 改几何必须手动 `refresh()`**：只有 `hide()/show()/removeChild()/removeAllChildren()/setBorder*` 内部自动刷新；`setWidth/setSize/setFlex*` 等都不刷新。
5. **运行时节点显隐用 `hide()/show()`**，不要用 `removeChild` —— 后者会销毁对象、让外部保存的 `Button*` 失效。
6. **DPI**：所有 setter 收**逻辑像素**（内部乘 dpi），`Node::x/y/w/h` 是**物理像素**。宿主窗口可能在与系统不同的缩放比的屏幕上，工具条一律以 `win->dpi` 为准。
7. **驶入 / 驶出**：工具类构造 -> `createNativeWindow()` -> `onCreated()`（此时 `body` 才可用）-> 末尾 `show()`。
8. **语言包是 UTF-16**：用 python 读写（`open(p,'rb').read().decode('utf-16')`），不要用普通文本编辑。

## 5. 开发规范

- **简洁优先**：不加未被要求的功能、不做一次性抽象、不为不会发生的异常写防护。
- **精准修改**：不顺手重构相邻代码；每一行改动都要能追溯到具体需求。
- **注释写"为什么"**：本仓库注释密度高且质量好，改动时维持这个水平——解释取舍背景、踩过的坑，而不是复述代码。
- 新增 `.cpp` / `.h` 必须同步登记到 `Src/ScreenCapture.vcxproj` 的 `ClCompile` / `ClInclude` 节点，**否则不会被编译**。
- 新增 ToolSub 选项若需要图标：不要猜 iconfont 里有没有对应码位。**优先用短文本**（如 `L"圆"`/`L"ABC"`）并跳过 `setFontFamily(L"icon")`，这样走微软雅黑必定有字形。

## 6. 当前实施路线

见 `Doc/竞品能力差距分析.md` 第七章（A–H 分组）。**本轮范围以 7.4「本轮范围裁剪」为唯一依据**：
已裁剪 D3 公式识别、D4 AI 翻译、macOS、E1 UI 元素级检测、F 组（FSCapture 方向编辑器）、G3/G4、H2/H4；H6 只做水印；**内置 OCR（G5）必做**。

当前进行：**C 组后半 + D 组**。已完成并提交（dev 分支，按里程碑分次提交）：

- A 组：A1 ToolCap 两行容器、A2 序号增强、A3 智能擦除、A4 文字旋转、A5 输出格式与自动保存
- B 组：B1 WinPin 多实例（地基已具备）、B2 历史截图 + 剪贴板历史（ShotHistory / WinHistory）、B3 内置 OCR（Ocr / WinOcr，Windows.Media.Ocr）
- C 组：C1 贴图来源（PinSource：剪贴板 / 文件 / 文字 / 颜色值）、C2 贴图属性（不透明度 / 圆角 / 锁定 / 穿透 / 标题）、
  C3 贴图组 / 缩略图 / 对齐跨屏 / 动图（AnimImage + WinPin 帧播放 + 播放暂停 + 重启续播）、
  C4 贴图持久化与重启恢复、C5 悬浮球 + 拖放贴图（WinBall，WM_DROPFILES）、
  C6 依次贴图 + PageUp/PageDown 前后预览 + Ctrl+M 贴边细条
- H6：文字水印（ShapeWatermark，居中 / 平铺 / 透明度 / 旋转）

新增的快捷键（贴图窗口内）：空格 = 动图播放 / 暂停；Ctrl+T = 缩略图模式；
Alt+方向 = 贴到屏幕边；Ctrl+Alt+左右 = 搬到相邻显示器。

待做：C5 的「Win+拖拽快速贴图」（需全局键盘钩子 + 全屏拖放层，风险较高，单独评估）、
D1（OCR 多语种）、D2（表格识别）、E2–E5、G1/G2、H1/H3/H5。

## 7. 验证

本机具备 MSVC BuildTools 2022（`D:\ProgramFiles\Microsoft Visual Studio\2022\BuildTools`）与 Windows SDK 10.0.26100，
但在受限沙箱里 `MSBuild.exe` / `cl.exe` 会被判定为 LOLBin 拦截，**无法在会话内完成编译验证**。
作者在本地 Visual Studio 里打开 `ScreenCapture.slnx` 编译 x64 即可；新增文件记得同步 vcxproj。
