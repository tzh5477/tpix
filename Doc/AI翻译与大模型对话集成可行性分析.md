# AI 翻译 / 大模型对话集成到 tpix —— 可行性分析

> 分析对象：`D:\SoftwareBackup\AutoHotkey\AI_Translator\AI_Translator.ahk`（AHK v2，v2.3）
> 分析结论形态：**仅评估，不实施**。本文不含任何代码改动建议的落地步骤。
> 日期：2026-10-06

---

## 1. 结论先行

**技术可行，且 tpix 的现有地基比预想的高得多。** 脚本依赖的九类底层能力（HTTPS、JSON、后台线程 + 回 UI、
全局热键、全局鼠标钩子、模拟键盘、剪贴板、多行输入控件、独立顶层浮层）在 tpix 里**已有现成实现或等价先例**，
真正需要从零造的核心只有两处：**服务端签名（HMAC-SHA256）** 与 **字符级全局键盘钩子**。

但有三个前置判断必须先摆上台面，不能绕过：

1. **范围决策与本项目既有裁决冲突。** `Doc/竞品能力差距分析.md` 在 2026-10-01 已由用户明确裁决
   **D4「AI 翻译」不做**，理由原文：「需持续在线服务投入 + 用户自填 key，非主干体验」，
   并注明「将来若确有需求，可走『按需下载插件』形态另行立项」（§5 P2、§7.4 裁剪表）。
   本次集成**等于重启该决策**，需要显式重新裁决，而不是当作新需求直接排期。
2. **脚本的定位与 tpix 的定位不同。** AHK 脚本是一个**系统级输入增强器**（监听全键盘、直接改写任意程序输入框）；
   tpix 是一个**截图 / 贴图工具**。把脚本能力"整包搬入"，会让 tpix 承担一套它原本不打算承担的全局输入职责。
3. **脚本的六条能力链价值不均。** 其中两条与 tpix 的截图 / OCR 优势天然咬合（应做），
   两条属通用系统工具（可做、但收益与风险需权衡），两条**建议明确不做**（见 §4.4、§7）。

**建议：按能力分层决策，先做 §4.1（截图取词翻译）与 §4.2（全局热键划译），暂缓 §4.4。**

---

## 2. 被分析脚本的能力拆解

脚本共 **1420 行**，可归纳为六条能力链 + 一套支撑件。

| # | 能力链 | 触发方式 | 关键依赖 | 备注 |
|---|---|---|---|---|
| C1 | **行内打字触发翻译** | 输入 `/t内容&&` | 全局字符级键盘钩子 + 回删 + 剪切板替换 | `InputHookManager` |
| C2 | **行内打字触发 AI** | 输入 `/i内容&&` | 同上 | 同上 |
| C3 | **快捷键划译** | `Alt+T`（选中则翻译，否则弹输入框） | 全局热键 + 模拟 `Ctrl+C` 取选中 + 弹窗 | `!t` / `ShowTranslateInputWindow` |
| C4 | **快捷键 AI 提问** | `Alt+I`（必须先选中） | 全局热键 + 取选中 + 输入系统提示词 | `!i` / `OpenAIResult` |
| C5 | **鼠标长按右键划译** | 选中文字后在选区上长按右键 ≥0.25s | 全局鼠标钩子 + 取选中 + 贴选区浮层 | `$RButton` / `RBtnHoldTranslate` |
| C6 | **大模型对话** | C2 / C4 / C5 的共同出口 | OpenAI 兼容 HTTP | `LLM.Call` |

支撑件：

- **服务端**：火山引擎翻译（`translate.volcengineapi.com`，Action=TranslateText，Version=2020-06-01），
  **AWS SigV4 风格 HMAC-SHA256 签名**；OpenAI 兼容大模型（`/chat/completions`）。
- **凭据**：`config.ini` + **Windows DPAPI**（`CryptProtectData`/`CryptUnprotectData`）加密 AK/SK/Key。
- **配置向导**：首次运行依次弹框要求填写火山 AK/SK、LLM base_url/api_key/model。
- **浮层交互**：贴选区右侧（放不下翻左侧、再贴屏幕边）、鼠标离开 6 秒关闭、点浮层外立即关闭、
  持续置顶、点击浮层复制译文。
- **杂项**：自研 JSON dump/parse、UTF-8 URL 编码、全角半角归一、IME 组合态判定、日志（明文落盘）。

**脚本内部值得注意的两点行为**（影响移植判断）：

- **火山返回原文即视为"未识别"，转大模型兜底**（典型场景是中文拼音输入）。这条兜底让"英文/拼音"都能出结果，
  但也意味着**任何一次识别失败都会多走一次慢请求**。
- **取选中一律靠模拟 `Ctrl+C`**（`GetSelectedText` 用 `ClipboardAll` 暂存 → 清空 → `^c` → 读回 → 还原）。
  这是全脚本**侵入性最强**的一处：它会短暂改写用户剪贴板，且依赖"目标程序响应 Ctrl+C"。

---

## 3. tpix 侧地基盘点（逐项对照）

| 需要的能力 | tpix 现状 | 判定 |
|---|---|---|
| **HTTPS 客户端** | `Update.cpp`：WinRT `Windows.Web.Http` + `HttpBaseProtocolFilter` + `fire_and_forget` + `resume_background()`，含超时取消与错误吞没范式 | ✅ 现成 |
| **JSON 解析 / 构造** | `Windows.Data.Json`（`Setting` / `Lang` / `Update` 均用）；`Update` 有 `JsonObject::TryParse` 兜底范式 | ✅ 现成 |
| **后台线程 → 回 UI** | `Ling::App::get()->dq.TryEnqueue(...)`；`std::thread`（定时自动截图）；`winrt::resume_background()` | ✅ 现成 |
| **全局热键** | `Setting.cpp` 的 `shortcutDefs[]` 表（配置键 → 消息 id → 默认组合）+ `regHotKey/unRegHotKey/onHotKey`（`RegisterHotKey`） | ✅ 加两项只改表 + `switch` |
| **全局鼠标钩子** | `GlobalMouse`（`WH_MOUSE_LL`，Win+拖拽取词/贴图）；`SelectPopup`、`WinWatermarkPanel` 同款 | ✅ 长按右键手势可直接扩展 |
| **字符级全局键盘钩子** | 仅 `CapVideo` 用了 `WH_KEYBOARD_LL`，且**只观察不吞**；无"捕获字符流 + 拼 buffer"的实现 | ⚠️ **核心缺口，见 §4.4** |
| **模拟键盘输入** | `Util::pasteToWindow` 走 `SendInput` 发**真键盘事件**（注释明说：只认真键盘事件，`WM_PASTE` 不够） | ✅ 现成 ⇒ `Ctrl+C` 同理可得 |
| **剪贴板读写** | `Util::readClipboard`（图/文判定）、`saveToClipboard`、`setHtmlToClipboard` | ⚠️ 有读写，但**无"暂存-还原整个剪贴板"的现成封装**，需自建 |
| **多行文本输入控件** | `Ling::TextBox`：多行、自动折行、滚轮滚动、占位符、**IME 组合态事件** `onChar/onIME`、焦点态 `onFocusChanged` | ✅ 聊天窗/结果框可直接用 |
| **独立顶层浮层窗口** | `SelectPopup`（`WS_EX_TOPMOST\|TOOLWINDOW\|NOACTIVATE` + `WH_MOUSE_LL` 判"点在外面"来收）；`WinWatermarkPanel`（独立顶层窗、按锚点摆位） | ✅ 划译浮层照抄这两套 |
| **语言包** | `Lang/*.json`（UTF-16 LE 带 BOM，`rc.exe` 编入 exe）；`Lang::get(L"key.path")` | ✅ 现成 |
| **配置持久化** | `Setting`（`JsonObject`，明文 `%appdata%\tpix\config.json`，可绿色版） | ⚠️ **明文**；需新增凭据保护 |
| **HMAC-SHA256 / 加密** | **全仓零 crypto 代码**（grep `bcrypt`/`crypt32`/`Crypt*` 无命中） | ⚠️ **核心缺口**，但有两条无新依赖的解法，见 §4.6 |
| **截图取词（OCR）** | 内置 `Ocr::recognize` / `recognizeWords`（`Windows.Media.Ocr`，**带词级坐标**）；`WinOcr` 结果窗已成型 | ✅ **tpix 独有优势** |

**关键结论：tpix 缺的不是"能不能做"，而是两块特定零件（字符级键盘钩子、加密/签名），
其余九成能力可直接复用既有实现或既有范式。**

---

## 4. 逐能力可行性判定

### 4.1 截图 / 贴图内取词翻译（★ 最契合，建议优先做）

**可行性：高。**

- tpix 已有**内置离线 OCR**（`Ocr::recognizeWords` 返回带框的词），这是 AHK 脚本**完全没有**的能力。
  AHK 只能翻译"用户手动选中的文字"，而 tpix 可以直接翻译**屏幕上任意一块区域**——
  不需要用户先在别的程序里选中，天然绕开了 §2 里"模拟 Ctrl+C 取选中"的全部侵入性与不确定性。
- 落点明确：`WinCap`（截图后）/ `WinOcr`（识别结果窗）/ `WinPin`（贴图标注）三处之一或全部。
  `WinOcr` 已具备"结果文本框 + 复制按钮 + 语言选择 + 多任务序号防串（`taskSeq`）"的骨架，
  加一个「翻译」按钮即是同构扩展。
- 结果呈现可复用 `WinOcr` 的两栏/浮层范式，或新建一个只读 `Ling::TextBox` 结果窗。

**待决问题**：翻译结果放**原图浮层**还是**独立窗口**？前者要对齐 C5 的浮层逻辑（§4.3），后者更简单。

### 4.2 全局热键划译（`Alt+T` 语义）

**可行性：中高。**

- 热键注册：`shortcutDefs[]` 加一行 + `onHotKey` 的 `switch` 加一个 `case`，是本仓**最成熟**的扩展点。
- 取选中：复用 `SendInput`（`pasteToWindow` 已证明类可用）；`Ctrl+C` 的键序列与 `Ctrl+V` 同理。
  需要新增：剪贴板整体暂存/还原（AHK 的 `ClipboardAll` 等价物），以及"目标程序是否响应"的失败兜底。
- 结果呈现：`Alt+T` 的弹窗可复用 `WinOcr` 的窗口骨架。

**风险点**：模拟 `Ctrl+C` 有**三类失败态**——目标程序不响应、目标选区为空、剪贴板被并发占用。
脚本用"剪贴板非空才当作有选中"来兜底，tpix 需给出等效且带提示的处理。

### 4.3 鼠标长按右键划译 + 贴选区浮层

**可行性：中。**

- 手势：`GlobalMouse` 已用 `WH_MOUSE_LL` 实现"按住修饰键 + 拖动"的手势判定，
  扩展出"`RButton` 按下 → 计时 0.25s → 判定长按/短按"在技术上完全同源。
  注意本仓已有约定：**框选期间要吞掉鼠标消息**（否则目标程序会跟着同一次拖动选中文字），
  短按必须**放行**系统右键菜单——脚本用 `Send("{RButton}")` 重放，tpix 侧需等价处理。
- 浮层：`SelectPopup`（判"点在外"就收）+ `WinWatermarkPanel`（独立顶层窗、按锚点摆位）
  两套范式合起来几乎就是 C5 浮层所需的全部。脚本的"贴选区右侧 / 翻左侧 / 贴屏幕边"
  定位算法（`MonitorWorkArea` + 工作区夹取）已有同款先例（`PositionWindow`、`SelectPopup` 的工作区判断）。
- **浮层窗口的既有坑已经记在本仓 MEMORY**：这类窗口**不能调 Ling 的 `setSize/setPosition/show`**
  （它们会真激活、抢焦点导致宿主 `onBlur` 收尾），必须自己合成
  `SetWindowPos(..., SWP_NOACTIVATE|SWP_SHOWWINDOW)`，并把 `WM_MOUSEACTIVATE` 答成 `MA_NOACTIVATE`。
  脚本的 `FloatTipTopMost()`（`SWP_NOACTIVATE` 只改 Z 序）正是同一条经验。

**风险点**：多显示器 / 混合 DPI 下的选区坐标与浮层定位；"长按阈值 0.25s"与系统右键菜单的
时序边界（脚本注释已承认"在选区外长按，浏览器会先取消选区导致取不到文字"）。

### 4.4 行内打字触发（`/t…&&` / `/i…&&`）—— **不建议移植**

**可行性：低-中（技术上可做，但代价与风险不成比例）。**

这是全脚本**最难、最侵入、也最不该照搬**的一条，原因有四：

1. **字符级全局键盘钩子是硬缺口。** AHK 的 `InputHook("V")` 能拿到**已过 IME 转换的字符流**；
   C++ 侧要做到同等效果，需 `WH_KEYBOARD_LL` + `ToUnicodeEx`/`GetKeyboardState` 自行翻译，
   **中文输入法的组合态（拼音上屏）会显著复杂化**。脚本为此专门写了 `IME_IsComposing()`
   与"全角转半角"，可见这一层的边界情况极多。
2. **它要求"观察全键盘 + 回删 + 回填"。** 即：放行字符 → 检测到 `&&` → `Send("{Backspace N}")` → 粘贴译文。
   这条链路会**改写用户正在输入的任意程序内容**，是纯粹的"输入法替代品"职责，
   与 tpix 的截图 / 贴图定位冲突。
3. **脚本自己要写一大堆兜底**：读焦点控件文本（`SendMessageTimeoutW`）失败时改走"全选复制"；
   hook 文本与控件文本比对（`TextStartsNormalized`）以防截断；防抖 400ms；触发延迟 200ms 等
   （`InputHookManager.Execute` 通读下来约 120 行，几乎每一步都在打补丁）。
   这些补丁的根因是"在别人的输入框里做文章"——**补丁越多，越说明这条路不该由截图工具来走**。
4. **用户感知差**：`/t` 这类前缀会与聊天软件的 `/` 命令、Markdown 输入冲突，脚本的 30 字符 buffer 与
   防抖窗口也说明误触发是常态。

**若确有需求**：脚本作者自己给出的备选形态是"按需下载插件"（§5 P2），
即做成独立小工具 / 可选插件，而不是内置进主程序。

### 4.5 大模型对话窗口

**可行性：高（但价值取决于产品决策）。**

- UI：`Ling::TextBox` 已是完整多行输入控件（折行、滚动、IME、`onChar/onIME`、占位符），
  一次"系统提示词 + 用户输入 + 结果 + 生成/复制"的三段式对话框，可直接照 `WinOcr`
  的窗口结构搭出来。脚本的 `LayoutAiWin`（1:2:4 分配剩余高度）在 Ling 里对应 yoga flex 布局，更简单。
- **缺口是"流式输出"**：脚本与 `Update.cpp` 走的都是**一次性响应**；
  若要 ChatGPT 式的逐字上屏，需改用 `HttpClient` 的 `GetInputStreamAsync` 做 SSE 增量读取，
  期间要处理"取消 / 换行 / 半截 UTF-8 字符"。这属于**新增**工作量，脚本里没有对应实现。
- **风险**：`onBlur → 收尾` 的双重触发、按键转发、失活窗口等，本仓 MEMORY 已记录多起同类崩溃，
  新建对话窗时必须复用既有结论（`finished` 标志、`onDestroy` 里先 `SelectPopup::close()` 等）。

### 4.6 服务端：火山签名 + OpenAI 兼容

**可行性：高，但需从零写两块。**

- **火山签名（HMAC-SHA256，SigV4 风格）**：tpix 无 crypto 代码，但**不必引入第三方库**，两条路：
  - **首选**：WinRT `Windows.Security.Cryptography.Core`（`MacAlgorithmProvider` +
    `CryptographicEngine`），随 `windowsapp.lib` 已在链接列表内，**零新增依赖**；
  - 备选：P/Invoke `bcrypt.dll`（脚本自己就是这么做的，165 行里大半是 BCrypt 样板）。
  算法本身（`kDate → kRegion → kService → kSigning → signature` 链）与脚本一一对应，无难点。
- **OpenAI 兼容**：标准 `POST {base_url}/chat/completions`，`messages[{role,content}]`，
  `Authorization: Bearer`。与 `Update.cpp` 的用法同构。
- **凭据保护**：脚本用 DPAPI。tpix 的 `config.json` 是明文，若照搬会导致 **API Key 明文落盘**。
  建议：key 不写进 `config.json`，单独经 DPAPI（P/Invoke `CryptProtectData`，需新增 `crypt32` 链接，
  或 WinRT `DataProtectionProvider(Local=user)`，零新增依赖）加密存一个独立文件。

---

## 5. 风险与不确定项

| 类别 | 风险 | 说明 |
|---|---|---|
| **决策** | 与本项目既有裁决冲突 | D4 已被明确裁为"不做"，重启需用户显式确认，并同步更新 `竞品能力差距分析.md` |
| **成本** | 持续在线服务 + 用户自填 key | 原文裁决理由；翻译走火山需用户自备 AK/SK，LLM 需自备 key |
| **安全** | API Key 明文落盘 | tpix 配置无加密；照搬脚本的 ini 思路会把 key 暴露在 `config.json` |
| **隐私** | 内容出网 + 明文日志 | 脚本把所有输入/输出**明文写进 `logs/*.log`**（`Logger.Write`）；这条**绝不可带入** tpix |
| **体验** | 全局注入键盘 | `Backspace` / `Ctrl+V` / `Ctrl+C` 会污染用户剪贴板与目标程序内容（§4.2、§4.4） |
| **兼容** | 剪贴板竞态 | 模拟 `Ctrl+C` 与用户的复制操作可能互相覆盖；需整体暂存/还原 |
| **兼容** | 输入法组合态 | C++ 侧需复刻"组合中不判定"的逻辑，否则拼音会被误触发 |
| **UI** | 多显示器 / 混合 DPI | 浮层定位、选区坐标换算，本仓已有多起 DPI 相关教训 |
| **稳定性** | 异步回调打到已销毁窗口 | 网络是异步的，窗口可能已被关闭；本仓有大量 use-after-free 历史（`WinBase` 析构不销毁 hwnd 等），回调必须挂 `onDestroy` 取消 |
| **一致性** | 另起一套 HTTP / 配置 | 会与 `Update.cpp`、`Setting` 的既有范式分家，应复用而非复制 |

---

## 6. 建议方案（分层 + 分阶段，供决策）

### 6.1 架构建议

- **新增内聚模块**（不散落到各窗口）：`Src/Ai/` 下放
  `Translate.*`（服务抽象：火山 / OpenAI 兼容）、`AiHttp.*`（薄封装 `Update.cpp` 那套 HttpClient 用法）、
  `AiCrypt.*`（HMAC-SHA256 + DPAPI）、`AiCredential.*`（密钥存取）。
- **不重复造 HTTP**：把 `Update.cpp` 里的「后台线程 + `HttpClient` + 超时取消 + 错误吞没」抽成小工具复用；
  但要**精准修改**——`Update` 是已上线功能，重构它属于风险动作，宁可先并行实现再评估收敛。
- **配置**：新增 `config.json` 的 `ai` 组（`url` / `model` / `enabled` 等非敏感项），
  **密钥单独加密存**（见 §4.6），与 `Setting` 的明文组分开。
- **生命周期**：所有网络任务持有"取消令牌"，窗口 `onDestroy` 时置位，回调进来先检查——避免访问违例。

### 6.2 分阶段（按"价值 / 风险比"排序）

| 阶段 | 内容 | 依赖 | 风险 |
|---|---|---|---|
| **P0 决策** | 重新裁决是否重启 D4、范围到哪一层、是否接受"用户自填 key" | 无 | — |
| **P1** | **截图 / 贴图取词翻译**：复用 `Ocr` + 新增翻译服务 + 结果窗 | OCR（已有） | 低 |
| **P2** | **全局热键划译**（`Alt+T` 语义）：热键 + `SendInput` 取选中 + 结果窗 | `shortcutDefs` | 中 |
| **P3** | **划译浮层**（长按右键）：`WH_MOUSE_LL` + 贴选区浮层 | `GlobalMouse`/`SelectPopup` | 中 |
| **P4** | **LLM 对话窗**（含可选流式输出） | `Ling::TextBox` | 中 |
| **✗** | **行内打字触发 `/t…&&`**：建议不做（或做成独立插件） | 字符级键盘钩子 | 高 |

### 6.3 落地前建议先做的三项技术验证（spike，各半天量级）

1. **WinRT Crypto 能否算出与脚本一致的火山签名**（拿脚本的 AK/SK 对拍一次真实请求）。
2. **DPAPI / DataProtectionProvider 加密存取一个字符串**在 tpix 进程内跑通。
3. **一个 `WS_EX_NOACTIVATE` 顶层浮层**（贴鼠标、点外关闭）在 Ling 下的行为——即 `SelectPopup` 范式的复用验证。

---

## 7. 明确的"不建议照搬"清单

1. **行内打字触发 `/t…&&` / `/i…&&`**（§4.4）——职责越界，边界情况极多，建议不做或做成独立插件。
2. **回删 + 自动粘贴替换输入框内容**（`Send("{Backspace N}")` + `^v`）——会改写用户正在编辑的内容。
3. **明文日志**（`logs/*.log` 记录全部输入输出）——隐私问题，**任何情况下都不要带入**。
4. **把 tpix 做成系统级输入法替代品**——与"截图 / 贴图工具"的定位冲突。

---

## 8. 结论

- **技术结论**：可行。九成地基 tpix 已有现成实现或等价范式，
  真正的核心缺口只有 **字符级全局键盘钩子** 与 **HMAC 签名 / 凭据加密** 两块；
  前者只在"行内打字触发"里才需要（该能力建议不做），后者有两条零新增依赖的解法。
- **产品结论**：**最该做的不是"照搬这个脚本"，而是"借它的思路强化 tpix 已有的 OCR 取词"**——
  用"截图/贴图内取词翻译"取代"模拟 Ctrl+C 抓选中文字"，体验更好、侵入性更低、可靠性更高。
- **下一步需要您裁决**：
  1. 是否重启 `竞品能力差距分析.md` 中的 **D4** 决策？
  2. 范围取到哪一层（P1 / P1+P2 / P1+P2+P3 / 含 P4）？
  3. 是否接受"用户自填 key"这一前提？
