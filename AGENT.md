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

当前进行：**E 组 → G/H 组**。已完成并提交（dev 分支，按里程碑分次提交）：

- A 组：A1 ToolCap 两行容器、A2 序号增强、A3 智能擦除、A4 文字旋转、A5 输出格式与自动保存
- B 组：B1 WinPin 多实例（地基已具备）、B2 历史截图 + 剪贴板历史（ShotHistory / WinHistory）、B3 内置 OCR（Ocr / WinOcr，Windows.Media.Ocr）
- C 组：C1 贴图来源（PinSource：剪贴板 / 文件 / 文字 / 颜色值）、C2 贴图属性（不透明度 / 圆角 / 锁定 / 穿透 / 标题）、
  C3 贴图组 / 缩略图 / 对齐跨屏 / 动图（AnimImage + WinPin 帧播放 + 播放暂停 + 重启续播）、
  C4 贴图持久化与重启恢复、C5 悬浮球 + 拖放贴图（WinBall，WM_DROPFILES）、
  C6 依次贴图 + PageUp/PageDown 前后预览 + Ctrl+M 贴边细条
- H6：文字水印（ShapeWatermark，居中 / 平铺 / 透明度 / 旋转）
- D1：OCR 多语种（Ocr::languages + 结果窗口语言按钮 + 设置页默认语言）
- E3：延时截图（WinDelay 倒计时窗口）、定时自动截图（App 里一条计时线程）
- E4：包含鼠标指针（Util::snapshotCursor + captureScreen 的 withCursor）
- E5：横向滚动截图（CapLong 支持滚动轴：转置灰度条带复用竖向匹配、`MOUSEEVENTF_HWHEEL`、
  成图尺寸用 `resultW × resultH`、配置的那个方向滚不动自动换一次向）
- H5：屏幕标尺 / 十字准线 / 屏幕聚焦（WinOverlay 一个铺满桌面的顶层窗口 + 托盘三项开关）
- H1（前半）：全局快捷键体系扩充 —— `Setting.cpp` 里一张 `shortcutDefs` 表管住
  「配置键名 → 消息 id → 默认组合」，可配的动作从 2 个扩到 8 个（新增历史记录、
  悬浮球、剪贴板贴图、三个屏幕辅助层）
- refactor(shape)（作者主导）：Canvas / CanvasHost 抽出画布宿主，ShapeRectBase / ShapeLineBase
  两个中间基类，马赛克与擦除拆成四变体；**Shape 层此后只认 Canvas，不认 WinPin**
- E2-1：手绘（自由多边形）区域 —— `CutMask` 上加一条 poly 通路（`startPoly/addPolyPoint/endPoly`），
  `maskRect` 仍是多边形的外接矩形（工具条布局 / 长截图 / 录屏 / 贴图一概不用改），
  形状只在导出时生效：`getCutImg` 用 `PushLayer` + 偶奇填充路径做遮罩，
  `getCutPixels` 走 `getCutImg()`，所以复制 / 存盘 / OCR / 扫码一并跟着生效
- E2-2：固定尺寸区域 —— `Setting::fixedSizePresets()`，按下即成框（单击也出图），
  越界时推原点而不是缩尺寸
- E2-3：多窗口与多级菜单 —— 根因是延时窗口 `SetFocus` 抢焦点把菜单点没了；
  改成 `WS_EX_NOACTIVATE`（代价是没有键盘，Esc 改成轮询 `GetAsyncKeyState(VK_ESCAPE) & 1`，
  取 `& 1` 而非 `& 0x8000`：倒计时一秒才走一次 tick，只看按住的话一次短按必然落在两次 tick 之间）
- G1（部分）：**录制暂停 + 鼠标点击可视化**。摄像头画中画未做 ——
  它要另起一条 MediaFoundation 摄像头采集管线，与桌面复制是两回事，单独评估
- G2：按键与鼠标操作录制 —— 新增 `KeyFx.hpp`（组合键用 `chord` 定格，取"此刻还按着的"会被
  最后松开的修饰键冲掉）、`ClickFx::Halo`（鼠标移动高亮）；
  MP4 与 GIF 两条管线共用这两份 inline 头，避免同一套逻辑在两处各自长歪
- H1 后半：配置导入导出 —— `Setting::exportConfig / importConfig`，设置页「配置备份与恢复」一行两个按钮。
  导出不带 `pin` 组（贴图是运行时状态）；导入是整份替换，解析失败一个字都不动。
  导入后要重来一遍的只有三类：热键、开机自启、语言
- H3 后半：复制后自动粘贴 —— `Util::snapshotForeground / pasteToWindow`。
  前台窗口与指针快照必须在建窗之前取；粘贴发的是真键盘事件而不是 `WM_PASTE`
- D2：表格识别 —— `Src/Table.h/.cpp`（slanet-plus 结构 + 系统 OCR 填字），OCR 结果窗口上
  多一个「表格 / 文字」切换按钮，复制时走 `Util::setHtmlToClipboard`（CF_HTML + 纯文本两份）。
  ORT 与模型怎么进来、前后处理为什么是这个数，见下面 D2 那节
- C5 余项：全局鼠标（PixPin「Win+拖拽」）—— `GlobalMouse`，`WH_MOUSE_LL` 钩子 +
  一个铺满桌面的框选层。左键拖动贴图 / 中键拖动复制 / 右键拖动认文字并复制。
  框选期间吞掉鼠标消息，带一个 100ms 定时器兜底钩子被系统摘掉的那种情况。默认关
- 循环按钮改下拉（作者提的交互问题：点一次切一档，档位一多要连点好几次）——
  新增 `Src/SelectPopup.h/.cpp`，见下面「下拉框」那节。覆盖标注工具条 7 处多档 +
  全部两态开关、设置页 6 处多档 + 全部开关、OCR 窗口语言与表格切换、贴图播放 / 暂停

新增的快捷键（贴图窗口内）：空格 = 动图播放 / 暂停；Ctrl+T = 缩略图模式；
Alt+方向 = 贴到屏幕边；Ctrl+Alt+左右 = 搬到相邻显示器。

### 待做（按此顺序推进，D2 放最后）

1. ✅ ~~**E2** 手绘区域 / 固定尺寸区域 / 多窗口与多级菜单~~（已完成）
2. ✅ ~~**G1** 录制暂停 + 鼠标点击可视化~~（已完成；**摄像头画中画未做**）
3. ✅ ~~**G2** 按键与鼠标操作录制~~（已完成）
4. ✅ ~~**H1 后半** 配置导入导出~~（已完成）
5. ✅ ~~**H3 后半** 自动粘贴到输入焦点~~（已完成）
6. ✅ ~~**C5 余项** Win+拖拽快速贴图~~（已完成，见「全局鼠标」）
7. **G1 余项** 摄像头画中画（需另起 MediaFoundation 摄像头采集管线，单独评估）
8. ✅ ~~**D2** 表格识别~~（已完成，见下）

### 标注工具（ShapeText / ShapeNumber / ShapeArrow）

三个工具按 pixpin 的手感对齐了一轮（提交 `24ab6ec`）。

- **文本（`ShapeText`）**
  - **字体**：工具条新增字体按钮（`ToolSub::makeFontBtn`），点开是**系统已装字体全表**
    （`IDWriteFontCollection` 枚举一次存成静态表；族名取 en-US 那份落盘，按钮上显示中文名，
    没中文名就退回族名）。落盘存的是**族名而不是下标** —— 下标会随机器上装的字体变化串味。
    渲染侧在 `makeTextLayout` 建完 layout 之后 `SetFontFamilyName`，编辑中的 `TextBox` 走
    `setFontFamily`。字体默认不加粗（`isTextBold` 初值本来就是 false）。
  - **滚轮调字号**：光标停在文字上时滚滚轮，一格两个逻辑像素，改完经
    `ToolSub::setShapeSliderVal(L"text", ...)` 夹值域并回写滑块（与矩形滚轮调线宽同一套）。
    字号变了要 `fitRectToText()` 把边框盒重新贴到文字上，否则文字会溢出原来的框。
  - **旋转手柄**：从"框正上方"挪到**右下角**，并改画成**圆弧 + 两端箭头**的括号
    （pixpin 那个旋转提示），不再是原来那个实心方块。静止方向变了，`mouseDrag` 里算出的
    是鼠标方向，要减掉 `restAngle` 才是"相对静止位置转了多少"。
- **序号（`ShapeNumber`）**
  - 外圈样式扩到**五种**：无尾圆 / 无尾方 / 无 / 圆+箭头 / 方+箭头，**默认无尾圆**
    （pixpin 的圈号没有尾巴）。带不带尾巴由 `hasTail()` 一处判定，几何、控制点、命中
    三处都看它 —— 无尾时 tip / mid 那两个控制点既不画也不响应。
  - **枚举顺序是按落盘兼容排的，别改**：这个值直接进 `config.json`（`number.ringStyle`），
    所以 0 / 1 / 2 仍是圆 / 方 / 无（圆与方只是按需求去掉了尾巴），带尾的两个新增在 3 / 4。
    把新增项插到中间，"无外圈"的老配置会变成"圆+箭头"。
  - 徽章**左侧**挂 `+` / `−` 两个小圆按钮（`updateValueBtns` 定位，恒在左、不跟着 `angle`
    转）。`hoverDraggerIndex` 用 3 / 4 表示这两枚，`mouseDown` 里按下即 `bumpVal` 并
    **直接返回、不进拖拽**。按钮位置在 `makePath` 末尾统一刷新 —— 所有会动 cx/cy/r 的
    路径都会调它，不用逐个补。
  - 编号来源改成**工具条上的「编号」输入框**（`ToolSub::takeNumberVal`，取完自增并落盘）。
    原来是"图上最大编号 + 1"，连删几个再画就会重号。撞号仍然级联顶号（`setValAndPush`）。
- **箭头（`ShapeArrow`）**
  - 新增**普通箭头**（平口尾、箭杆首尾等粗、头上接三角）并设为默认；原来的**尖尾渐变**
    （尾部收成一个点、箭杆由细到粗）留作第二项。两者只在尾部不同，在 `makeArrow` 里
    分 `arrowStyle` 走。切样式时 `makeSelectBtn` 的 `onPicked` 调
    `WinPin::onToolStyleChanged()`，选中的那个箭头立刻换形状。

**图标字体**：这一轮又补两个字形 —— **E909**（普通箭头）/ **E90A**（尖尾箭头），共 42 个。
`Doc/tools/mkicons.py` 改成了**可重复执行**（已存在的码位 / 字形名直接跳过），再跑不会重复插入。

**`SelectPopup`**：`show` 多了一个 `minW`（逻辑像素）。字体名能长到十几个字符，
按按钮宽度开会把「Microsoft YaHei UI Light」这种截掉。

### 悬浮球（WinBall）

折叠态是**贴着屏幕边的一条 100×5 的红线**（不是原来的圆球），鼠标移上去展开成一排操作
图标，移开收回。贴左 / 右边时竖排，贴顶边时横排。拖那条细线换边，松手吸附离得最近的
一条边（左 / 右 / 上；下边不参与 —— 压在任务栏那一带没意义）。

- **操作项**：`Src/BallAction.h` 一张表管住 id / 图标码位 / 语言键，表里的顺序就是
  展开条上的顺序。设置页「悬浮球」分组（`WinSettingBall`）照着它逐行生成勾选框，
  勾完立刻 `WinBall::reload()` 重建 —— 节点树是照着旧配置建的，不重建就新旧混在一起。
  默认只勾八个高频项，17 项全开会拖到屏幕外。
- **带参数的两项**（滚动截图方向、延时截图秒数）：左键按当前设置直接跑，**右键弹
  `SelectPopup` 改参数**。延时秒数是悬浮球自己的一份（`ball.delaySec`，1~10），
  没跟设置页那个 0/2/3/5/10 的档位表共用 —— 两套档位对不上，硬合并只会互相改坏。
- **窗口几何**：折叠与展开都围着"贴边锚点 + 沿边中点"对齐，所以细线在展开前后不跳，
  鼠标从细线滑到图标也始终在窗口内（不会中途触发 WM_MOUSELEAVE 收起）。
  展开时是先撑大窗口再让图标参与布局，反过来的话会先挤在细线里重排一遍、看着闪一下。
- **弹层与收起的冲突**：弹层订阅了宿主的 `onMoved`（宿主一挪就自己关），而收起会挪动
  本窗口 —— 鼠标移向弹层时若照常收起，用户刚点开的列表会凭空消失。所以 `onMove` 里
  发现 `SelectPopup::isOpen()` 就先不收，挂一个 200ms 定时器等它关掉再收
  （id `0x5200`）。
- **图标位置提示**：`Tip` 新增 `Side`（Above / Below / Left / Right）。气泡不能往屏幕
  外弹，所以贴顶边时挂到按钮下面、贴右边时挂到左边，方向按 `edge` 定。
- **`SelectPopup` 的坐标**：它原来拿 `anchor->x/y`（**窗口内**坐标）当屏幕坐标用，
  在设置页里因为窗口偏移只有 1px 看不出来，悬浮球贴在屏幕边上就会偏出近两千像素。
  已改成加上 `owner->x/y`，并夹进工作区。

**图标字体**：`Src/Res/iconfont.ttf` 原来只有 30 个字形，缺时钟 / 剪贴板 / 历史 / 标尺 /
十字 / 聚光 / 设置 / 两个「依次贴」。补齐后的码位是 **E900~E908**（见 `BallAction.h`）。
生成脚本在 `Doc/tools/mkicons.py`：em=1024、主体落在 x[96,928] y[-24,880]、视觉中心
(512,392)、线宽 60~72；**外轮廓顺时针、孔洞逆时针**，非零环绕下才有洞，互相重叠的
形状必须同向（否则重叠处会被挖掉）。

### 下拉框（SelectPopup）

原先一批按钮是「点一次切一档」的循环按钮，档位一多就得连点好几次才转到想要的那个。
统一改成点一下弹列表、一次点中。`Src/SelectPopup.h/.cpp`：

- **弹的是独立顶层窗口**，不挂在宿主里。标注工具条只有一行高，设置页靠底部的行
  列表也会伸到窗口外面 —— 宿主窗口装不下它。样式：`WS_EX_TOPMOST | WS_EX_TOOLWINDOW
  | WS_EX_NOACTIVATE`，**不抢焦点**（否则文本框丢光标、贴图窗口可能失焦自收）。
- **收起靠一个 `WH_MOUSE_LL` 钩子**：只有它能在宿主窗口之外也收得到点击，
  宿主自己的 `onMouseDown` 只能看见自己这一亩地。点在弹出按钮上不算点在外面 ——
  那一下要留给按钮自己收起（所以记的是按钮的屏幕矩形而不是指针：宿主重建按钮后指针会野）。
- 位置默认往下弹，底下放不下翻到按钮上方。用按钮**所在显示器的工作区**判断，
  不用虚拟桌面整体 —— 副屏在左上时后者会把翻转判错。
- 超过 320 逻辑像素就放进 `ScrollerBox` 滚动（OCR 语言列表最长）。
- 两态开关的列表是两项**图标**（iconfont 里的 `\ue687` 叉与 `\ue688` 勾），
  和按钮上显示的是同一对，所以不用另起一套「开 / 关」译名。关在前开在后，下标能当 bool 用。
- 收起只销毁窗口句柄，C++ 对象推迟到下一轮消息循环 —— 收起多半是从某次点击的栈上发起的。
  重复 `close()` 是安全的：`WM_DESTROY` 不会二次触发。

接入点：设置页 `WinSettingCommon::makeSelectBtn / makeSwitchBtn`；
工具条 `ToolSub::makeSelectBtn / showOnOff`（原来的 `makeCycleBtn` 已删）；
`WinOcr` 的语言与表格切换；`ToolSub` 的贴图播放 / 暂停。
`App::dispose`、`ToolSub::beginTool / hideTools`、`WinSetting` 切菜单与关窗都会收掉还开着的列表。

### D2 表格识别（已完成）

`Src/Table.h/.cpp` + `Util::setHtmlToClipboard` + `WinOcr` 上的「表格 / 文字」切换按钮。
结构走 **slanet-plus**（RapidTable 的 ONNX 模型），格子里的字走系统 OCR。

- **依赖**：ORT 1.30 已解到 `D:\sdk\onnxruntime`（`build\native\include` +
  `runtimes\win-x64\native`），工程按 gifski 那套绝对路径挂法接进去，另外加了一条
  PostBuildEvent 把 `onnxruntime*.dll` 拷到 `$(OutDir)` —— 它是动态库，不拷跑不起来。
- **模型**：`slanet-plus.onnx`（7.4MB）**首次使用时**下到 `%appdata%\ScreenCapture`，
  先写 `.downloading` 再改名（半截文件不能骗过下次的存在检查）。下不来就明说，不静默。
- **前后处理（与 RapidTable 逐行对齐，别改）**：
  - 缩放到 488×488 的 letterbox，长边缩到 488、**短边截断**（不四舍五入），右下**补归一化
    之后的 0**（不是黑）—— 顺序反了结果就漂。双线性取的是 cv2 那套映射
    `(dst+0.5)*src/dst-0.5`。
  - 归一化 `/255` → 减 mean `[0.485,0.456,0.406]` → 除 std `[0.229,0.224,0.225]`，
    逐通道套在 **BGR** 上（模型是拿 cv2 训的，换 RGB 反而不对）。排布 CHW。
  - 输出两个：最后一维 8 的是格子框、50 的是结构概率（按维度认，不依赖输出名）。
  - **格子框 = 归一化坐标 × max(h, w)**，直接就是原图像素坐标。这是 RapidTable 里
    `_bbox_decode` + `rescale_cell_bboxes` 两步的等价写法（实测最大差 3e-5），
    省掉 ratio / shape_list 那一串中间量。
  - token 表在模型自定义元数据的 `character` 里（48 个，前后加 sos / eos = 50），
    从模型里读而不是写死。格子的框在**开头那一帧**上（`<td` 或 `<td></td>`）。
  - 合并单元格按占位摊平成规整矩阵（文字只在左上角那一格），所以输出里没有
    rowspan / colspan 属性 —— 换来的是 HTML 与 TSV 行列对齐。
- **填字**：整张图只认一次 `Ocr::recognizeWords`（词级带框），按中心点落格，比逐格裁开
  去认快几十倍。中文之间不加空格、英文之间加。
- **复制**：`Util::setHtmlToClipboard` 同时放 **CF_HTML**（Word / Excel 粘出真表）和
  **CF_UNICODETEXT**（tab 分隔，记事本 / Excel 也认）。CF_HTML 是 UTF-8，
  头里四个 10 位偏移先按 0 拼整段、量好位置再回填。

> 记一笔 E4 的坑：`App::takeScreenShot` 与 `Util::captureScreen` 曾经是两份 GDI 抓屏代码，
> 现在统一走后者。以后新增"改抓屏行为"的能力，只改 `Util::captureScreen` 一处。

> 记一笔 Ling 的坑：`WinBase` 的析构**不销毁 hwnd**，所以关窗口必须走 `close()`，
> 不能 `unique_ptr.reset()` —— 后者会留下一个还在收定时器 / 鼠标消息的野窗口（use-after-free）。
> 换窗口（关掉旧的开新的）要用 `pendingMode` 这类接力：`close()` → `onDestroy` 的延迟任务里再 `new`。

## 7. 验证

本机具备 MSVC BuildTools 2022（`D:\ProgramFiles\Microsoft Visual Studio\2022\BuildTools`）与 Windows SDK 10.0.26100，
**编译是通的**（2026-10-01 由作者验证：编译通过并修掉了缺陷）。编译命令：

```
cmd /c "call \"D:\ProgramFiles\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat\" >nul && \"D:\ProgramFiles\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe\" \"<proj>\" /p:PlatformToolset=v143 /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo"
```

- `<proj>` 取 `Src\ScreenCapture.vcxproj`（不必走 `.slnx`）。
- 工程默认工具集是 `v145`（VS 2026），本机只有 VS 2022 的 `14.44`，所以**必须显式传 `/p:PlatformToolset=v143`**，否则报找不到工具集。
- 产出在 `Src\x64\Release\`。

> 注：AI 会话这一侧的工具策略仍会把 `MSBuild.exe` / `cmd.exe` 判为 LOLBin 拦截，
> 所以完整构建只能由**作者执行**。链接期与运行期的问题务必在真机上跑一遍再收工。

### 7.1 会话内自证：cl.exe /Zs 语法检查

`cl.exe` 本身不在拦截名单里，可以绕开 `cmd.exe` 直接调，做**只过前端**的语法 / 语义检查
（`/Zs` 不生成 obj，不留任何产物）。这一层能挡掉绝大多数低级错误（拼错的成员名、
参数类型不匹配、模板实例化失败），比纯静态审查可靠得多。

```bash
export MSYS_NO_PATHCONV=1   # 关键：否则 Git Bash 会把 /nologo 这类开关当成路径改写
MSVC="D:/ProgramFiles/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207"
SDK="C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0"
INCLUDE="$MSVC/include;$MSVC/atlmfc/include;$SDK/ucrt;$SDK/um;$SDK/shared;$SDK/winrt;$SDK/cppwinrt;D:/sdk/gifski;D:/sdk/onnxruntime/build/native/include;D:/project/Ling;<仓库>/Src" \
"$MSVC/bin/Hostx64/x64/cl.exe" /nologo /Zs /std:c++20 /EHsc /utf-8 \
  /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DWIN32 /DNDEBUG /D_CONSOLE /D_UNICODE /DUNICODE /D_WIN64 \
  /Tp"Src/Win/CapVideo.cpp"
```

- `/Tp` 是必须的：直接给 `.hpp` 会被当成头文件跳过。
- `NOMINMAX` / `WIN32_LEAN_AND_MEAN` 缺了会报一堆 `C2589 "(":"::"右边的非法标记`
  （`std::max` 撞上 minwindef 的宏），那是命令行没对齐工程的 `PreprocessorDefinitions`，不是代码问题。
- `atlmfc/include` 要给，否则 `atlbase.h` 找不到。
- 期末版本号（14.44.35207）与 SDK 版本号会随机器变，跑之前先 `ls` 确认。
- 头文件单独查：`/Tp"Src/Win/KeyFx.hpp"`（不依赖 pch 的那些可以直接过）。
