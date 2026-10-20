# 选文工具三项优化 — 设计文档

日期：2026-10-20
状态：已确认（方案 A（字符级选区）+ 光影只扫 1 次 1 秒 + 空格收放都不退选文态，作者拍板）

## 背景

贴图窗口「选文」开关（`ToolMain` 上的 `textSelect`）三个问题：

1. **点开没有任何反馈**：`setTextSelect(true)` 只刷新画面，OCR 已在后台认完时画面毫无变化，
   用户不知道是否进入了选文态、也不知道能不能选；
2. **很难精确选中**：选区粒度是**词级**（`caretIndexAt` 按词中点判插入点），而内置模型里
   「中文一行 = 一个词」，中文只能整行选；英文选不到单词内部；行判定只拿行内**第一个词**的
   高度当行界，字号混排时点这行会判到别行；拖动前没有任何 hover 高亮，点之前看不见会选中什么；
3. **选文态按空格收不掉工具条**：`WinPin::onKey` 的选文分支只认 Ctrl+C / Ctrl+A / ESC 就
   提前 `return`，走不到 `VK_SPACE` 那条。

## 关键事实（实现前必须知道）

- 内置 OCR 模型（PP-OCRv6，默认）在 `Ocr::recognizeWords(..., chars)` 里**同时给出字符级框**
  （`WinPin::ocrChars`，现在只有智能打码在用）：每项一个字，格子按相邻峰值中点均分、**铺满整行无缝隙**。
- 字符框与词框**同源同序**（都出自 `OcrModel::splitWords`）：一个词框正好等于它所含各字框的并集；
  词 = 一行 rec 文本按空格切开的段，字符框跳过空格 —— 所以**按词序消费，词 k 吃掉 `words[k].text.size()` 个字符框**，总数必然相等。
- 系统 OCR 回落路径与大模型路径**没有字符框**（`ocrChars` 为空）→ 自动退回词级，行为与现状一致。
- 词内天然不含空格，空格只出现在词与词之间 → 复制文本时**只在跨词处**用现有 `ocrNeedSpace` 判空格，词内直接拼接，拼不出半个词。
- 选区下标现语义是「词下标 0..ocrWords.size()」，本方案改为「**单元下标** 0..selUnits.size()」。
- 空格收放工具条的另一处坑：`setToolsVisible(true)` 末尾会 `toolMain->selectTool(L"geom")`，
  而 `selectTool` 开头那句 `win->setTextSelect(false)` 会把选文关掉 —— 选文态下必须跳过预选。

## 变更点

### 1. 选区单元表（方案 A：字符级选区）

`WinPin.h` 新增：

```cpp
// 一个"选区单元"：选区的最小粒度。有字符级框时一个字一格（from/to 是它在所属词里的
// 字符区间），没有时整个词一格（from=0, to=词长）—— 后者与改造前的词级行为完全一致
struct SelUnit {
    int word{ 0 };          // 属于 ocrWords 的第几个词
    int from{ 0 }, to{ 0 }; // 词内字符区间 [from, to)
    float x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 };   // 标注坐标系里的矩形
};
std::vector<SelUnit> selUnits;   // 选区单元表（与 ocrWords 同序，按行、行内从左到右）
std::vector<OcrLine> selLineUnits; // 每行的单元区间 [first, first+count)，与 ocrLines 一一对应
int selHover{ -1 };              // 光标底下那个单元；-1 = 没有
```

`applyOcrWords` 在 `ocrLines` 分组完成后调 `buildSelUnits()`；`clearOcr()` 里
`selUnits / selLineUnits` 一并清、`selHover = -1`（选区下标本就在这里复位）。

`buildSelUnits()` 规则：

- `ocrWords` 里 `text.empty()` 的词不建单元（无内容、画出来也只是个空高亮）；
- **无字符框**（`ocrChars` 为空，或字符框总数 ≠ 各词字符数之和）→ 整词一格，rect 取词框；
- **有字符框** → 逐词消费：词 k 的第 i 个字符取 `ocrChars[累计 + i]` 的矩形与下标；
- 顺带算出 `selLineUnits`（按 `ocrLines` 的词区间映射到单元区间）。

各函数改造（全部在 `WinPin.cpp`）：

- `caretIndexAt`：返回单元下标。行判定改用**整行包围盒**（该行所有词的 min top / max bottom），
  不再拿第一个词的高度当行界；行内按单元中点判插入点（与现在词中点同一套手感）。
- `wordIndexAt` → 改名 `unitAt(imgPos)`：返回**含该点的单元**下标，无则 -1。
- `onDown` 双击分支：命中单元 u → 向左右扫同 `word` 的单元区间 = 该词的单元范围，
  `selAnchor = 词首单元, selCur = 词尾后一格`；没命中 → 与单击一致取 `caretIndexAt`。
  `onDown` 与 `onMove` 里各补一次 `selHover = unitAt(...)`，松手瞬间 hover 不停在旧位置。
- `selectedText`：按单元拼 —— 换行判定沿用现有「词落在第几行」查法；**只在跨词处**判
  `ocrNeedSpace(prev词尾, 本词头, 词间相对间隙)`（与改造前同一判据），词内 substr 直接拼。
- `paintTextSelect`：按单元矩形铺蓝底。hover 规则：**没有选区且未在拖选时**才画，
  画的就是 `selHover` 那一格（复用 `selectBrush`，不新建刷子）——选区一旦存在就不再画，
  免得两层高亮叠花。
- `onMove`（选文态、未拖动）：`unitAt` 结果变化时更新 `selHover` 并 `refresh()`；
  拖动中照旧只动 `selCur`。
- `onKey` Ctrl+A：`selCur = (int)selUnits.size()`；ESC / 右键清选区逻辑不变（下标语义自洽）。

### 2. 扫描光影（点开选文后从左往右 1 秒一趟）

- `WinPin.h`：`ULONGLONG scanStartAt{ 0 };`（0 = 没在扫）。定时器 id 用 **104**（100~103 已占用）。
- `setTextSelect(true)`：`scanStartAt = GetTickCount64(); setTimer(16, 104);`
  `setTextSelect(false)`：`scanStartAt = 0; killTimer(104);`（不残留半条光带）。
- `onTimerCB` 新增 104 分支：超过 1000ms → `scanStartAt = 0; killTimer(104);`，然后 `refreshNow()`；
  没到点也 `refreshNow()`（每帧重绘，SetTimer(16) 约 15.6ms 一跳，1 秒约 60+ 帧）。
- `onCreated` 建刷子处新建**渐变刷**（stop 集合 + 线性渐变刷各建一次，此后每帧只改起止点）。
- `layout()` 在 `SetTransform(Identity)` 之后、`paintTitle` 之前调 `paintScanFx(ctx)`：
  按窗口坐标画（窗口即图，不跟标注缩放），带宽 `w * 0.28`，左沿从 `-band` 线性走到 `w`
  （`left = -band + p * (w + band)`，p = 已过时长 / 1000），渐变起止点设在带的左右沿。
- 颜色：**主题色**（`Setting::getThemeColor()`，与选区高亮同色系）三段 stop —
  `0 → alpha 0`、`0.42 → alpha 0.45`、`1 → alpha 0`。用主题色而不是白光：
  白色亮带在白底截图上完全隐形，主题色深浅底图都看得见。
- OCR 是否认完**不参与**：只扫这一趟，认得慢时顶部那句「正在识别文字…」照旧负责说明。

### 3. 选文态空格收放工具条

- `onKey` 选文分支加一条 `VK_SPACE → setToolsVisible(!isToolsVisible())`，
  放在 ESC 之前（互斥键，顺序无关紧要，但挨着写清楚）。
- `setToolsVisible(true)` 末尾：`if (textSelect) return;`（不预选几何工具）——
  选文态 `curId` 本来就是空的（`setTextSelect` 进来时 `cancelSelect` 过），
  `setSideBarVisible(true)` 内部已经 `layoutTools()`，无需补排。
  **收起**方向不用改：`cancelSelect()` 不碰 `textSelect`，收起天然不退选文态。

### 4. 不动的部分

- OCR 触发链路（后台识别、`startOcr`、大模型合成整词）、`ocrNeedSpace` 判据本身；
- 智能打码（继续用 `ocrChars` 的 `cx`）、表格识别、`Ocr::recognizeWords` 签名；
- 双击 = 选词、右键 = 清选区 / 收工具条、Ctrl+C / Ctrl+A / ESC 语义；
- 工具条布局、`ToolMain` 按钮样式与 `setToggle` 回报。

## 验收标准

1. MSBuild 全量编译 0 错误、除 Ling 外 0 新警告；冒烟 3 秒存活。
2. 点「选文」→ 1 秒内一条主题色光带从左扫到右后消失；再点关掉立即无残留。
3. 中文正文（一行一个词）能从行中间开始选、只选半行 / 一句话；选区高亮逐字贴合。
4. 英文能选中单词的一部分；双击仍选中整个词。
5. 字号混排的行，点行内任意位置选中的是这一行（不再串行）。
6. 未拖选时鼠标划过文字，光标下那一格有淡高亮；开始拖选 / 已有选区后不画。
7. 复制出的文本：中文无多余空格、英文词间保留空格、跨行保留换行（与改造前一致）。
8. 选文态按空格 → 工具条整组收起，选文态仍在（可继续拖选）；再按空格 → 工具条回来，
   选文按钮仍亮、选文态不退出。
9. 系统 OCR / 大模型路径（无字符框）：选区退回词级，表现与改造前一致，不劣化。
10. 非选文态的既有行为回归：标注工具、框选、Ctrl+C 三条复制路径、ESC 两段式关窗。
