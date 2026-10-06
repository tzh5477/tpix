# 贴图边线回显优化 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 藏起的贴图 hover 边线时回显到该线旁边（fscapture 样式），条改 30×4，离开后延 800ms 再收回。

**Architecture:** 全部改动集中在 `PinHiddenBar`（条几何、摆位、宽限计时）两个文件；`WinPin` 不动——回显挪窗由条侧 `setPosition` 完成，原位用 `prevX/prevY` 在 peek 生命周期内记忆，「隐藏/显示」按钮语义靠"收回时挪回或就地藏"自然保持。

**Tech Stack:** C++20 / Win32 / Ling 框架（`setPosition` 收物理像素；`setWidth/setHeight/setPadding` 收逻辑像素，内部乘 dpi）/ MSBuild（v143, Release x64）

## Global Constraints

- 编译命令：`cmd /c "%TEMP%\opencode\main_build.cmd" > <日志> 2>&1`，退出码必须 0；唯一允许警告 = `D:\project\Ling\include\Util.h(44,45) C4244`（外部框架）。
- 进程名 `tpix.exe`（`Src\x64\Release\tpix.exe`）；编译前 `Get-Process tpix | Stop-Process`；冒烟 = 启动 3 秒存活后杀掉。
- `barX/barY` 是物理像素（源自 `SPI_GETWORKAREA` 工作区，构造函数里 `(barThick + pad * 2) * dpi` 的换算可证）；`x/y` 成员与 `setPosition` 同单位（物理）。
- 单位换算一律沿用现有模式：`(int)(逻辑值 * dpi)`。
- 摆位缝距 `peekGap = 4` 逻辑像素（spec 第 2 节）；宽限 `peekGraceMs = 800`（spec 第 4 节）。
- 语言：提交信息与代码注释用简体中文，风格对齐仓库现有长注释（讲清"为什么"）。
- 本项目无自动化测试框架——每个任务的验证 = 编译 0 错误 + 该任务列出的手工核对步骤。

---

### Task 1: 条尺寸 50×2 → 30×4

**Files:**
- Modify: `Src/Win/PinHiddenBar.h`（常量 + 两处注释）
- Modify: `Src/Win/PinHiddenBar.cpp`（三处过期注释）

**Interfaces:**
- Consumes: 无
- Produces: `barLong = 30.f`、`barThick = 4.f`（Task 2 的摆位公式直接用这两个常量）

- [ ] **Step 1: 改常量与头文件注释**

`PinHiddenBar.h` 第 6-10 行形态注释中「50px 长、2px 宽的细线」改为「30px 长、4px 宽的细线」；第 50-53 行：

```cpp
	// 30px 长、4px 厚（逻辑像素，交给 Ling 的 setter 时由其内部乘 dpi）。
	// 竖排时长边是高、横排时长边是宽，所以按"长 / 厚"命名，不叫宽高
	static constexpr float barLong{ 30.f };
	static constexpr float barThick{ 4.f };
```

- [ ] **Step 2: 同步 .cpp 过期注释**

- `rebuild()` 内「长边 50、厚 2。横排时这两条掉个个儿……」→「长边 30、厚 4。横排时这两条掉个个儿……」
- 同函数 `setFlexShrink` 行上「条只有 2 逻辑像素厚」→「条只有 4 逻辑像素厚」
- `barIndexAt` 上方「条只有 2 逻辑像素宽」→「条只有 4 逻辑像素宽」

（只改注释里的数字文字，不动逻辑。）

- [ ] **Step 3: 编译**

Run（PowerShell）:
```powershell
Get-Process tpix -ErrorAction SilentlyContinue | Stop-Process -Force
$log = "$env:TEMP\opencode\peek_t1.log"
cmd /c "`"$env:TEMP\opencode\main_build.cmd`" > `"$log`" 2>&1"
$LASTEXITCODE
Select-String -Path $log -Pattern 'error ' | Select-Object -First 10
```
Expected: `0`，无 error 行。

- [ ] **Step 4: 手工核对**

启动 `tpix.exe` → 截一张图进贴图 → 拖到屏幕左边缘藏起来 → 目测：条长为原来的 60%（30 vs 50）、厚了一倍（4 vs 2）；hover 条仍能回显（此任务回显仍是旧逻辑=回原位，属预期）。

- [ ] **Step 5: Commit**

```powershell
git add Src/Win/PinHiddenBar.h Src/Win/PinHiddenBar.cpp
git commit -m "style(pin): 隐藏边线条 50x2 改 30x4"
```

---

### Task 2: 回显摆位贴线 + 原位记忆

**Files:**
- Modify: `Src/Win/PinHiddenBar.h`（新增成员与私有方法声明）
- Modify: `Src/Win/PinHiddenBar.cpp`（`reveal` / `conceal` 改造 + 新函数）

**Interfaces:**
- Consumes: Task 1 的 `barLong/barThick`；`WinPin::x/y/w/h`、`setPosition(int,int)`（public，WinBase 继承）、`WinPin::peek(bool)`（现有）、`WinPin::isBusy()`（现有）
- Produces: `reveal` 内先记原位再挪摆位；`conceal` 按"是否还在摆位"决定挪回或就地藏——Task 3 只在 `onTimerCB` 动刀，不碰这两个函数的摆位逻辑

- [ ] **Step 1: 头文件加成员与声明**

`PinHiddenBar.h` 私有区（`peek` 成员附近）新增：

```cpp
	// peek 期间的原位记忆（物理像素）。reveal 记下、conceal 用它把没被拖动的图挪回去；
	// 被拖走的图就地藏，窗口自己的 x/y 自然成为新原位 —— 「显示」按钮 show() 当前 x/y 即可
	int prevX{ 0 }, prevY{ 0 };
	// 本次 peek 的摆位（物理像素）。收回时拿它跟当前位置比，判定"用户拖过没有"。
	// 必须在 reveal 时存下来：conceal 时条序号可能已因增删重建而对不上
	int peekX{ 0 }, peekY{ 0 };
```

私有方法区新增声明：

```cpp
	// 第 index 条线旁边的摆位（物理像素，不钳位 —— 钳位需要贴图宽高，只有 reveal 拿得到）
	POINT calcPeekPos(int index) const;
```

- [ ] **Step 2: 实现 calcPeekPos**

`PinHiddenBar.cpp` 新增（放在 `reveal` 前）：

```cpp
// 摆位 = 贴着第 index 条线：左条图放线右侧、图顶对齐线顶；顶条图放线下方、图左对齐线左。
// 缝 4 逻辑像素。大图用工作区往回钳（与建条同源的 SPI_GETWORKAREA 主工作区）
POINT PinHiddenBar::calcPeekPos(int index) const
{
	const auto px = (float)barX, py = (float)barY;
	const auto thick = barThick * dpi;
	const auto step = (barLong + gapW) * dpi;
	const auto gap = (int)std::lround(peekGap * dpi);
	// 条的起点 + 单条厚度 + 缝，就是图贴边的那一侧
	// （钳位不在这里做：要用贴图的 w/h，本函数只有 index，钳进 reveal）
	return isHorizontal()
		? POINT{ (int)std::lround(px + index * step), (int)std::lround(py + thick) + gap }
		: POINT{ (int)std::lround(px + thick) + gap, (int)std::lround(py + index * step) };
}
```

头文件常量区补一个（`pad` 旁）：

```cpp
	// 回显贴图与那条线之间的缝（逻辑像素）
	static constexpr float peekGap{ 4.f };
```

钳位逻辑放进 `reveal`（Step 3 一并给出），用贴图窗口自身的 `target->w/target->h`（float，物理）。

- [ ] **Step 3: 改造 reveal / conceal**

`reveal` 替换为：

```cpp
void PinHiddenBar::reveal(int index)
{
	auto pins = WinPin::getHiddenPins(edge);
	if (index < 0 || index >= (int)pins.size()) return;
	auto target = pins[(size_t)index];
	if (peek == target) return;
	conceal();
	peek = target;
	// 原位与摆位都记在这一次（conceal 时条可能已重建，序号不再可靠，所以要存）
	prevX = target->x;
	prevY = target->y;
	auto p = calcPeekPos(index);
	// 大图钳回工作区：图比屏幕还宽 / 高时，摆位不能开出边外
	RECT wa{};
	SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
	peekX = std::clamp((int)p.x, (int)wa.left, std::max((int)wa.left, (int)(wa.right - target->w)));
	peekY = std::clamp((int)p.y, (int)wa.top, std::max((int)wa.top, (int)(wa.bottom - target->h)));
	target->setPosition(peekX, peekY);
	target->peek(true);
	// show() 会把那张贴图提到 topmost 组的最前（组内后显示的在上），正好压住本窗口。
	// 条被压在底下就再也 hover 不到了，所以这里把它提回来
	SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	setTimer(tickMs, tickId);
}
```

`conceal` 替换为：

```cpp
void PinHiddenBar::conceal()
{
	killTimer(tickId);
	if (peek) {
		// 还停在摆位上 = 用户没拖过 → 挪回原位再藏，「显示」按钮与既往行为一致；
		// 拖走了就就地藏 —— 窗口自己的 x/y 就是新原位，拖动手感不丢
		if (peek->x == peekX && peek->y == peekY) peek->setPosition(prevX, prevY);
		peek->peek(false);
		peek = nullptr;
	}
}
```

- [ ] **Step 4: 编译**

Run: 同 Task 1 Step 3 命令（日志改 `peek_t2.log`）。
Expected: 退出码 `0`，无 error 行。

- [ ] **Step 5: 手工核对（spec 验收 3/4/6/7）**

1. 左边藏两张（不同颜色）→ hover 第 1 条：图开在屏幕左侧、图顶对齐第 1 条线顶、距条约 4px（含 pad）；hover 第 2 条：第 2 张开在第 2 条线旁。
2. 藏一张**大图**（近全屏）→ hover：图被钳在工作区内，不出右边/下边。
3. hover 出来**不拖**，移开等收回 → 用「隐藏」按钮的逆操作（再点一次/显示）→ 图回隐藏前位置。
4. hover 出来**拖到屏幕中间**，移开等收回 → 图在中间消失；再 hover 同一条 → 图仍开在线旁；此时取消隐藏 → 图落在中间。
5. 顶边同序验证（横条、图线下方、左对齐）。

- [ ] **Step 6: Commit**

```powershell
git add Src/Win/PinHiddenBar.h Src/Win/PinHiddenBar.cpp
git commit -m "feat(pin): 回显贴图贴到所 hover 边线旁，收回时保住原位/拖动位语义"
```

---

### Task 3: 离开 800ms 宽限

**Files:**
- Modify: `Src/Win/PinHiddenBar.h`（常量 + `leaveAt` 成员）
- Modify: `Src/Win/PinHiddenBar.cpp`（`onTimerCB`）

**Interfaces:**
- Consumes: Task 2 的 `reveal`/`conceal`、现有 `isOverPeek()`、`peek->isBusy()`
- Produces: `leaveAt` 状态机（0=在场）；`reveal` 里清零确保每次回显重新计时

- [ ] **Step 1: 头文件加常量与成员**

`tickMs/tickId` 旁：

```cpp
	// 离开宽限：第一次发现鼠标离开后，连续这么久仍不在场才收回去（spec 第 4 节）
	static constexpr UINT peekGraceMs{ 800 };
	// 离开时刻（GetTickCount）。0 = 在场。每跳轮询里记 / 清
	DWORD leaveAt{ 0 };
```

- [ ] **Step 2: 改 onTimerCB**

```cpp
void PinHiddenBar::onTimerCB(UINT id)
{
	if (id != tickId) return;
	if (!peek) {
		killTimer(tickId);
		return;
	}
	// 在场判定：鼠标压在图 / 两条工具条 / 本条上，或图正被拖着 / 正在编辑 —— 都视作在场
	if (peek->isBusy() || isOverPeek()) {
		leaveAt = 0;
		return;
	}
	// 第一跳发现离开 → 记时刻；连续离开满 800ms 才收回。中途回来 leaveAt 清零，重新计时
	if (leaveAt == 0) {
		leaveAt = GetTickCount();
		return;
	}
	if (GetTickCount() - leaveAt >= peekGraceMs) conceal();
}
```

同时 `reveal` 里 `setTimer(tickMs, tickId);` 之前补一行 `leaveAt = 0;`（每次回显重新开始计时）。

`PinHiddenBar.h` 第 59-60 行 `tickMs` 注释从「复核间隔：露出来之后每隔这么久看一眼鼠标还在不在，不在就收回去」改为「复核间隔：露出来之后每隔这么久看一眼鼠标还在不在；离开判定见 onTimerCB（连续离开满 800ms 才收）」。

- [ ] **Step 3: 编译**

Run: 同 Task 1 Step 3 命令（日志改 `peek_t3.log`）。
Expected: 退出码 `0`，无 error 行。

- [ ] **Step 4: 手工核对（spec 验收 5）**

1. hover 条回显 → 鼠标移开到空白处且**不碰图** → 秒表/体感：图在约 0.8~1.1s 后收回（不是立即，也不是 3s）。
2. hover 回显 → 移开 400ms 又移回图上 → 图不收回；再移开满 800ms → 收回。
3. 回显后直接去点工具条按钮 → 按住期间不收回，移开满 800ms 收回。

- [ ] **Step 5: Commit**

```powershell
git add Src/Win/PinHiddenBar.h Src/Win/PinHiddenBar.cpp
git commit -m "feat(pin): 边线回显离开后延 800ms 再收，中途回场清零重计"
```

---

### Task 4: 全量回归 + 推送

**Files:**
- 无代码改动

**Interfaces:**
- Consumes: Task 1-3 全部
- Produces: 验收闭环

- [ ] **Step 1: 全量 Rebuild + 冒烟**

```powershell
Get-Process tpix -ErrorAction SilentlyContinue | Stop-Process -Force
$log = "$env:TEMP\opencode\peek_final.log"
cmd /c "`"$env:TEMP\opencode\main_build.cmd`" > `"$log`" 2>&1"
$LASTEXITCODE
Select-String -Path $log -Pattern 'error ' | Select-Object -First 10
$p = Start-Process "F:\personal\project\github\tpix\Src\x64\Release\tpix.exe" -PassThru
Start-Sleep 3
if ($p.HasExited) { "冒烟失败 code=$($p.ExitCode)" } else { "冒烟通过"; Stop-Process -Id $p.Id -Force }
```
Expected: 退出码 `0`、无 error、冒烟通过。

- [ ] **Step 2: spec 验收 1-8 逐条过**

按 `docs/superpowers/specs/2026-10-06-pin-edge-peek-design.md`「验收标准」8 条逐项手工核对（几何、摆位、钳位、宽限、两条收回路径、颜色/条数/双条窗/按钮回归）。

- [ ] **Step 3: Commit spec 与本计划（若尚未提交）**

```powershell
git add docs/superpowers/specs/2026-10-06-pin-edge-peek-design.md docs/superpowers/plans/2026-10-06-pin-edge-peek.md
git commit -m "docs: 贴图边线回显优化的设计与实现计划"
```

- [ ] **Step 4: 推送**

```powershell
git push origin dev
```
Expected: `... dev -> dev`，`git status` 干净。
