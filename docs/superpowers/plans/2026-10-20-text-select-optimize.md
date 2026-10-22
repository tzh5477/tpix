# 选文工具三项优化 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 选文工具支持字符级精确选中、点开时 1 秒扫描光影、选文态空格收放工具条且不退选文态。

**Architecture:** 全部改动落在 `Src/Win/WinPin.h/.cpp`（+1 处 `ToolMain` 无关）。识别结果回填时把词表切成「选区单元」（有字符框逐字、无字符框整词），选区下标从词下标换成单元下标；扫描光影走现有 `setTimer` + `refreshNow` 帧动画；空格键在 `onKey` 选文分支补一条并让 `setToolsVisible(true)` 在选文态跳过预选几何工具。

**Tech Stack:** C++20 / Win32 / Direct2D，MSVC v143（BuildTools 2022），无测试框架 —— 验证 = 编译 0 错 0 新警告 + 冒烟启动。

**Spec:** `docs/superpowers/specs/2026-10-20-text-select-optimize-design.md`

## Global Constraints

- 编译命令（AGENT.md §7.1，.cmd 固化法）：`%TEMP%\opencode\main_build.cmd`，MSBuild `Src\tpix.vcxproj /p:PlatformToolset=v143 /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo`；编译前 `Get-Process tpix | Stop-Process -Force`。
- 成功判据：退出码 0、日志 `error ` 零条、尾部有 `tpix.vcxproj -> ...Src\x64\Release\tpix.exe`；警告预期只有 Ling 的 `C4244` 一条。
- 冒烟：启动 exe，3 秒存活即过。
- 定时器 id 只剩 **104** 可用（100~103 已占用）。
- 注释写「为什么」，密度与本文件既有水平一致；不顺手重构；不新增语言包文案。
- 语言：注释与提交信息全用简体中文，提交格式沿用 `type(scope): 中文描述`。

---

### Task 1: 选文态空格收放工具条

**Files:**
- Modify: `Src/Win/WinPin.cpp`（`onKey` 选文分支 ~L3748、`setToolsVisible` ~L2152）

**Interfaces:**
- Consumes: `WinPin::isToolsVisible()` / `setToolsVisible(bool)` / `textSelect` 成员（均已存在）。
- Produces: 无新接口，纯行为变更。

- [ ] **Step 1: onKey 选文分支补空格**

在 `if (textSelect) { ... }` 块内、`else if (key == VK_ESCAPE)` 之前插入：

```cpp
		else if (key == VK_SPACE) {
			// 选文态下空格照旧收放工具条（作者：选文模式里按空格工具条收不掉）。
			// 收起方向天然不退选文态（cancelSelect 不碰 textSelect），**请回来**那方向
			// 要靠 setToolsVisible 里跳过预选几何工具 —— selectTool 开头会把选文关掉
			setToolsVisible(!isToolsVisible());
		}
```

- [ ] **Step 2: setToolsVisible(true) 在选文态跳过预选**

`setToolsVisible` 末行 `toolMain->selectTool(L"geom");` 改为：

```cpp
	// 选文态把工具条请回来时**不能**预选几何工具：selectTool 开头那句
	// setTextSelect(false) 会顺手把选文关掉（要求是收放都不退选文态）。
	// 选文态 curId 本来就是空的，ToolSub 也已收起，摆位由下面 setSideBarVisible
	// 里的 layoutTools 负责，什么都不用补
	if (textSelect) return;
	// 由它带着跑 layoutTools，位置一并摆好
	toolMain->selectTool(L"geom");
```

- [ ] **Step 3: 编译验证**

Run: 构建 .cmd + MSBuild（增量），`Select-String -Path $log -Pattern 'error '`
Expected: 零 error、仅 Ling C4244 警告、有 `-> ...tpix.exe` 产物行。

- [ ] **Step 4: Commit**

```bash
git add Src/Win/WinPin.cpp
git commit -m "fix(pin): 选文模式下按空格收不掉工具条，收放都不退选文态"
```

---

### Task 2: 扫描光影（点开选文 1 秒一趟）

**Files:**
- Modify: `Src/Win/WinPin.h`（成员 ~L679 选区成员附近）
- Modify: `Src/Win/WinPin.cpp`（`onCreated` 建刷子 ~L2260、`setTextSelect` ~L3230、`onTimerCB` ~L3027、`layout` ~L2333、新增 `paintScanFx`）

**Interfaces:**
- Consumes: `setTimer(ms,id)` / `killTimer(id)` / `refreshNow()`、`Setting::getThemeColor()`、`Ling::D2D::get()->d2dFactory`。
- Produces: `void WinPin::paintScanFx(ID2D1DeviceContext* ctx)`；成员 `scanStartAt`、`brushScan`、`scanStops`。

- [ ] **Step 1: WinPin.h 加成员**

选区成员附近（`selDragging` 声明之后）加：

```cpp
	// ---- 选文的扫描光影 ----
	// 点开选文那一刻起算的起点 tick（GetTickCount64），0 = 没在扫。
	// 只扫一趟 1 秒：它回答的是"这一下点进选文了吗"，OCR 认得慢不慢与它无关
	ULONGLONG scanStartAt{ 0 };
	// 光带的渐变资源：stop 集合只建一次，光带每帧只挪起止点（见 paintScanFx）
	Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> scanStops;
	Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brushScan;
	// 画光带（选文开关打开后的 1 秒里）。调用方在窗口坐标层（变换已归位）
	void paintScanFx(ID2D1DeviceContext* ctx);
```

- [ ] **Step 2: onCreated 建渐变刷**

`selectBrush` 创建之后加（此处已有 `d2d->deviceContext` 与 `d2d->d2dFactory`）：

```cpp
    // 选文扫描光带的渐变：主题色，中段最亮、两端透明。stop 集合建一次就够，
    // 每帧只 SetStartPoint/SetEndPoint —— 一趟 1 秒约 60 帧，每帧现建刷子是白扔
    {
        D2D1_GRADIENT_STOP stops[]{
            { 0.f,    D2D1::ColorF(Setting::getThemeColor(), 0.f) },
            { 0.42f,  D2D1::ColorF(Setting::getThemeColor(), 0.45f) },
            { 1.f,    D2D1::ColorF(Setting::getThemeColor(), 0.f) },
        };
        d2d->deviceContext->CreateGradientStopCollection(stops, ARRAYSIZE(stops),
            scanStops.GetAddressOf());
        D2D1_LINEAR_GRADIENT_BRUSH_DESC desc{
            .gradientStops = scanStops.Get(),
            .startPoint = { 0.f, 0.f },
            .endPoint = { 1.f, 0.f },
        };
        d2d->deviceContext->CreateLinearGradientBrush(&desc, brushScan.GetAddressOf());
    }
```

- [ ] **Step 3: setTextSelect 起表 / 收表**

`setTextSelect(true)` 分支末尾（`startOcr()` 那句前后皆可，取 `toolMain->setToggle` 之前）加：

```cpp
		// 扫描光影：1 秒一趟，从左扫到右。它只回答"这一下点进选文了吗" ——
		// 认字认没认完由顶部那句"正在识别文字…"说明，两条信息互不掺和
		scanStartAt = GetTickCount64();
		setTimer(16, 104);
```

`else`（关闭）分支加：

```cpp
		if (scanStartAt) { scanStartAt = 0; killTimer(104); }
```

- [ ] **Step 4: onTimerCB 加 104 分支**

放在 `id == 103` 分支之后、`if (id != 100) return;` 之前：

```cpp
	if (id == 104) {   // 选文的扫描光影：1 秒一趟，到点收表；每帧重画一次
		if (GetTickCount64() - scanStartAt >= 1000) {
			scanStartAt = 0;
			killTimer(104);
		}
		refreshNow();
		return;
	}
```

- [ ] **Step 5: 实现 paintScanFx**

紧挨 `paintTextSelect` 之后加：

```cpp
// 点开选文那一秒，一条主题色光带从左扫到右。窗口坐标画（客户区即图，不跟标注缩放），
// 带宽取窗口宽的 28%，中段最亮两端透明 —— 进出场靠渐变自己柔掉，不用额外缓动。
// 用主题色不用白光：白底截图上白色亮带一整条都是隐形的
void WinPin::paintScanFx(ID2D1DeviceContext* ctx)
{
	if (!scanStartAt || !brushScan) return;
	const float p = (float)(GetTickCount64() - scanStartAt) / 1000.f;
	const float band = w * 0.28f;
	const float left = -band + p * (w + band);
	brushScan->SetStartPoint({ left, 0.f });
	brushScan->SetEndPoint({ left + band, 0.f });
	ctx->FillRectangle(D2D1::RectF(left, 0.f, left + band, (float)h), brushScan.Get());
}
```

- [ ] **Step 6: layout 里挂上**

`paintCropHandles(ctx);` 之后、`paintTitle(ctx);` 之前加：

```cpp
	// 选文的扫描光影：属于窗口装饰层（不跟标注缩放），压在边框之上、标题与提示之下
	if (textSelect) paintScanFx(ctx);
```

- [ ] **Step 7: 编译验证**（同 Task 1 Step 3 命令）
Expected: 零 error、仅 Ling C4244。

- [ ] **Step 8: Commit**

```bash
git add Src/Win/WinPin.h Src/Win/WinPin.cpp
git commit -m "feat(pin): 点开选文后 1 秒主题色光影从左扫到右"
```

---

### Task 3: 字符级选区（方案 A）

**Files:**
- Modify: `Src/Win/WinPin.h`（选区成员 ~L679、方法声明 ~L315）
- Modify: `Src/Win/WinPin.cpp`（`clearOcr`/`applyOcrWords` ~L3476、`caretIndexAt`/`wordIndexAt`/`selectedText`/`paintTextSelect` ~L3569、`onDown` ~L2452、`onMove` ~L2722、`onKey` Ctrl+A ~L3753）

**Interfaces:**
- Consumes: `ocrWords` / `ocrChars` / `ocrLines`（既有）、`ocrNeedSpace`（既有）。
- Produces:
  - `struct SelUnit`（头文件内）、成员 `selUnits` / `selLineUnits` / `selHover`
  - `void WinPin::buildSelUnits();`（私有）
  - `int WinPin::caretIndexAt(const POINT&) const` → 返回**单元**下标（签名不变）
  - `int WinPin::unitAt(const POINT&) const`（取代 `wordIndexAt`）

- [ ] **Step 1: WinPin.h 加单元结构与成员**

`struct OcrLine` 声明之前加结构，选区下标成员旁加三个成员：

```cpp
	// 一个"选区单元"：选区的最小粒度。有字符级框时一个字一格（from/to 是它在所属词里
	// 的字符区间）；没有时整个词一格（from=0, to=词长）—— 后者与改造前的词级行为一致。
	// 单元严格按 ocrWords 的顺序排：按行、行内从左到右，所以下标有序才有"选中从 A 到 B"
	struct SelUnit
	{
		int word{ 0 };            // 属于 ocrWords 的第几个词
		int from{ 0 }, to{ 0 };   // 词内字符区间 [from, to)
		float x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 };   // 标注坐标系里的矩形
	};
```

```cpp
	// 选区单元表 + 每行的单元区间（与 ocrLines 一一对应）。识别结果回填时建，清识别时清
	std::vector<SelUnit> selUnits;
	std::vector<OcrLine> selLineUnits;
	// 光标底下那一格（无选区时画淡高亮）；-1 = 没有
	int selHover{ -1 };
```

方法声明：`caretIndexAt` 注释里的「0..ocrWords.size()」改为「0..selUnits.size()」，并把
`int wordIndexAt(const POINT& imgPos) const;` 换成 `int unitAt(const POINT& imgPos) const;`，
新增 `void buildSelUnits();`（与 `applyOcrWords` 相邻声明）。

- [ ] **Step 2: 实现 buildSelUnits（clearOcr 之后、applyOcrWords 之前）**

```cpp
// 识别结果 → 选区单元表。有字符级框就一个字一格（选文于是能选到半个词 / 半行中文），
// 没有就整个词一格（系统 OCR 与大模型那两条路，行为与改造前完全一致）。
//
// 字符框与词框同出 OcrModel::splitWords：词是一行 rec 文本按空格切开的段、字符框跳过
// 空格，所以"词 k 吃掉 words[k].text.size() 个字符框"逐个消费必然严丝合缝 —— 总数对不上
// 说明这份字符框与词表不是一批货，整表退回词级，宁可粗也不能错位
void WinPin::buildSelUnits()
{
	selUnits.clear();
	selLineUnits.clear();
	size_t sum{ 0 };
	for (auto const& word : ocrWords) sum += word.text.size();
	const bool byChar = !ocrChars.empty() && ocrChars.size() == sum;
	size_t ci{ 0 };   // 下一个要消费的字符框
	for (int i = 0; i < (int)ocrWords.size(); ++i) {
		auto const& word = ocrWords[i];
		if (word.text.empty()) continue;   // 没内容的词不占选区（画出来只是个空高亮）
		if (byChar) {
			for (size_t k = 0; k < word.text.size(); ++k) {
				auto const& ch = ocrChars[ci++];
				selUnits.push_back({ i, (int)k, (int)k + 1, ch.x, ch.y, ch.w, ch.h });
			}
		}
		else {
			selUnits.push_back({ i, 0, (int)word.text.size(), word.x, word.y, word.w, word.h });
		}
	}
	// 行 → 单元区间。单元与词同序，按每行的词数把单元切段即可
	size_t u{ 0 };
	for (auto const& line : ocrLines) {
		size_t end = u;
		for (int i = line.first; i < line.first + line.count; ++i) {
			auto const& word = ocrWords[i];
			// 空词没建单元，与上面那句 continue 对齐
			end += word.text.empty() ? 0 : word.text.size();
		}
		selLineUnits.push_back({ (int)u, (int)(end - u) });
		u = end;
	}
}
```

- [ ] **Step 3: 接进回填与清理**

`clearOcr()` 内 `selDragging = false;` 之后加：

```cpp
	selUnits.clear();
	selLineUnits.clear();
	selHover = -1;
```

`applyOcrWords()` 内 `ocrLines` 分组循环结束之后、`refresh();` 之前加：

```cpp
	// 单元表必须在行分组之后建：它要按 ocrLines 切行区间
	buildSelUnits();
```

- [ ] **Step 4: caretIndexAt 改单元级 + 整行包围盒**

整函数替换为：

```cpp
// 标注坐标 → 选区的"插入点"下标（0..selUnits.size()）。与文本编辑器同一套：按阅读顺序
// 数下来，落在字与字的缝里、行尾空白上也能定出一个位置，不必非要命中某一个字。
// 行界取**整行包围盒**：拿行内第一个词的高度当行界时，字号混排的行里点在正文字上
// 会被判到下一行去（作者报的"点这行选到别行"就是这么来的）
int WinPin::caretIndexAt(const POINT& imgPos) const
{
	if (selLineUnits.empty()) return 0;
	for (int n = 0; n < (int)ocrLines.size(); ++n) {
		auto const& line = ocrLines[n];
		auto const& first = ocrWords[line.first];
		float top = first.y, bottom = first.y + first.h;
		for (int i = line.first + 1; i < line.first + line.count; ++i) {
			top = std::min(top, ocrWords[i].y);
			bottom = std::max(bottom, ocrWords[i].y + ocrWords[i].h);
		}
		if (imgPos.y < top) return selLineUnits[n].first;   // 在这一行上面 → 行首
		if (imgPos.y > bottom) continue;                    // 还在更下面的行里，接着找
		auto const& seg = selLineUnits[n];
		for (int u = seg.first; u < seg.first + seg.count; ++u) {
			// 以字的中点为界：过了一半就算走到下一格，与拖选文字的手感一致
			if (imgPos.x < selUnits[u].x + selUnits[u].w / 2.f) return u;
		}
		return seg.first + seg.count;   // 在行尾右边 → 这一行末尾
	}
	return (int)selUnits.size();
}
```

- [ ] **Step 5: wordIndexAt → unitAt**

整函数替换为：

```cpp
// 命中检验：光标底下是哪个**单元**。双击拿它定"选中这一个词"（见 onDown），hover 拿它
// 定要高亮哪一格。矩形单元比整词框细得多 —— 双击落在词缝上时不再整个词都算命中
int WinPin::unitAt(const POINT& imgPos) const
{
	for (int u = 0; u < (int)selUnits.size(); ++u) {
		auto const& s = selUnits[u];
		if (imgPos.x >= s.x && imgPos.x <= s.x + s.w
			&& imgPos.y >= s.y && imgPos.y <= s.y + s.h) return u;
	}
	return -1;
}
```

- [ ] **Step 6: selectedText 按单元拼**

函数体内的 `ocrWords[i]` 逐词遍历改为逐**单元**遍历（换行 / 空格判据不动）：

```cpp
std::wstring WinPin::selectedText() const
{
	if (!hasSelection() || selUnits.empty()) return {};
	const int lo = std::min(selAnchor, selCur);
	const int hi = std::min(std::max(selAnchor, selCur), (int)selUnits.size());
	std::wstring text;
	int lastWord{ -1 }, lastLine{ -1 };
	for (int u = lo; u < hi; ++u) {
		const auto& unit = selUnits[u];
		const auto& word = ocrWords[unit.word];
		// 这个单元落在第几行。ocrLines 按阅读顺序排，找出词落在哪一段里
		int lineNo{ -1 };
		for (int n = 0; n < (int)ocrLines.size(); ++n) {
			auto& line = ocrLines[n];
			if (unit.word >= line.first && unit.word < line.first + line.count) { lineNo = n; break; }
		}
		if (lastWord >= 0) {
			if (lineNo != lastLine) text += L'\n';
			// 空格只在**跨词**处判：词内天然没有空格，逐字判反而会把宽字距排版拆出空格来。
			// 判据仍是 ocrNeedSpace + 词间相对间隙，与改造前一字不差
			else if (unit.word != lastWord) {
				const auto& prev = ocrWords[lastWord];
				const float rel = (word.x - (prev.x + prev.w)) / std::max(prev.h, word.h);
				if (!prev.text.empty() && ocrNeedSpace(prev.text.back(), word.text.front(), rel))
					text += L' ';
			}
		}
		// 选区从词中间开始时，首个单元只拼它后面的那半截
		text += word.text.substr(unit.from, unit.to - unit.from);
		lastWord = unit.word;
		lastLine = lineNo;
	}
	return text;
}
```

- [ ] **Step 7: paintTextSelect 画单元 + hover**

```cpp
// 选中的单元铺一层半透明蓝底。调用方还在标注坐标系的变换里，所以它跟着缩放、剪裁一起走。
// 没有选区时光标底下那格画个淡高亮：拖之前先看得见"会选中什么"；选区一旦存在就不画，
// 免得两层高亮叠在一起反而看不清边界
void WinPin::paintTextSelect(ID2D1DeviceContext* ctx)
{
	if (!selectBrush) return;
	if (!hasSelection()) {
		if (selDragging || selHover < 0 || selHover >= (int)selUnits.size()) return;
		auto& s = selUnits[selHover];
		if (s.w > 0 && s.h > 0)
			ctx->FillRectangle(D2D1::RectF(s.x, s.y, s.x + s.w, s.y + s.h), selectBrush.Get());
		return;
	}
	const int lo = std::min(selAnchor, selCur);
	const int hi = std::min(std::max(selAnchor, selCur), (int)selUnits.size());
	for (int u = lo; u < hi; ++u) {
		auto& s = selUnits[u];
		if (s.w <= 0 || s.h <= 0) continue;
		ctx->FillRectangle(D2D1::RectF(s.x, s.y, s.x + s.w, s.y + s.h), selectBrush.Get());
	}
}
```

- [ ] **Step 8: onDown 双击与按下处换 unitAt / 同步 hover**

选文态左键分支整体替换为：

```cpp
	if (textSelect && !isRight) {
		auto imgPos = toImgPos(pos);
		// 双击 = 选中光标底下这一个词。要先认，否则按下的第一下已经先把它缩成一个插入点了
		if (takeDoubleClick()) {
			auto u = unitAt(imgPos);
			if (u >= 0) {
				// 单元与词同序，向两侧扫到词边界就是这个词的单元区间
				int first = u, last = u;
				while (first > 0 && selUnits[first - 1].word == selUnits[u].word) --first;
				while (last + 1 < (int)selUnits.size() && selUnits[last + 1].word == selUnits[u].word) ++last;
				selAnchor = first;
				selCur = last + 1;
			}
			else { selAnchor = selCur = caretIndexAt(imgPos); }
			lastDownTime = 0;   // 第三下不该再凑成一次双击
		}
		else {
			selAnchor = selCur = caretIndexAt(imgPos);
		}
		selDragging = true;     // 双击之后接着拖也能继续扩选
		// hover 是"没按着时"的预览，按下就换成真的选区；松手回到没选区时它得指着当前这格
		selHover = unitAt(imgPos);
		isMouseDown = true;
		hasDragged = false;
		SetCapture(hwnd);
		refresh();
		return;
	}
```

- [ ] **Step 9: onMove 未拖动时更新 hover**

选文态分支替换为：

```cpp
	if (textSelect) {
		if (!selDragging) {
			// 没按着：把光标底下那格标出来（没变化就不重画，mousemove 很密）
			auto u = unitAt(toImgPos(pos));
			if (u != selHover) { selHover = u; refresh(); }
			return;
		}
		// 按着就是在扩大 / 缩小选区（插入点跟着鼠标走）
		selCur = caretIndexAt(toImgPos(pos));
		refreshNow();
		return;
	}
```

- [ ] **Step 10: onKey Ctrl+A 换单元总数**

`selCur = (int)ocrWords.size();` 改为 `selCur = (int)selUnits.size();`。

- [ ] **Step 11: 编译验证**（同 Task 1 Step 3 命令）
Expected: 零 error、仅 Ling C4244。

- [ ] **Step 12: Commit**

```bash
git add Src/Win/WinPin.h Src/Win/WinPin.cpp
git commit -m "feat(pin): 选文选区细化到单字，行判定改整行包围盒并加 hover 高亮"
```

---

### Task 4: 全量验证与收尾

**Files:** 无新改动（只验证 + 文档提交）

- [ ] **Step 1: 全量 Rebuild + 日志判读**

杀 tpix 进程 → `/t:Rebuild` 全量 → 判读：退出码 0、`error ` 零条、仅 Ling C4244 警告、尾部 `-> ...tpix.exe`。

- [ ] **Step 2: 冒烟**

```powershell
$p = Start-Process "F:\personal\project\github\tpix\Src\x64\Release\tpix.exe" -PassThru
Start-Sleep 3
if ($p.HasExited) { "失败 code=$($p.ExitCode)" } else { "OK"; Stop-Process -Id $p.Id -Force }
```
Expected: `OK`。

- [ ] **Step 3: 提交方案与计划文档**

```bash
git add docs/superpowers/specs/2026-10-20-text-select-optimize-design.md docs/superpowers/plans/2026-10-20-text-select-optimize.md
git commit -m "docs: 选文工具三项优化的设计与实施计划"
```

- [ ] **Step 4: 自查清单（对照 spec 验收标准 1~10）**

逐条核对代码路径是否都已落地；功能手感项（2/3/4/5/6/7/8/9/10）留给作者真机确认，回复中明确列出。
