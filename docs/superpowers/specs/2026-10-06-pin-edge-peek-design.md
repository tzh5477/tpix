# 贴图边线回显优化 — 设计文档

日期：2026-10-06
状态：已确认（方案 A + 800ms 宽限，作者拍板）

## 背景

`feat(pin)` 已支持把贴图拖到屏幕左 / 上边线隐藏（`PinHiddenBar` 书签条）。现状两个问题：

1. hover 边线回显的贴图恢复的是**藏之前的原位**，离边线很远——鼠标离开边线后根本来不及移过去操作（fscapture 的行为是回显贴图就贴在边线旁边）；
2. 边线尺寸 50px × 2px，要求改 30px × 4px。

## 变更点

### 1. 条尺寸

`PinHiddenBar.h`：`barLong 50.f → 30.f`、`barThick 2.f → 4.f`（逻辑像素，Ling setter 内部乘 dpi）。头文件顶部形态描述注释同步改。`gapW`（2）、`pad`（4）不变。

### 2. 回显摆位（hover 哪条线，图开在哪条旁边）

hover 第 i 条线（i 从 0 起）时，把贴图窗口挪到该线旁边再 `show()`：

- **左条**（竖线）：`x = barX + barThick + 4`，`y = barY + i × (barLong + gapW)`
- **顶条**（横线）：`y = barY + barThick + 4`，`x = barX + i × (barLong + gapW)`
- 摆位后钳制在**工作区内**（图比屏幕大时往回收，复用 `layoutTools` 同款 clamp 思路），多显示器取条所在工作区（与建条同源）。
- 坐标单位与 `WinPin::x/y` 现有单位保持一致（实现时确认一次单位换算，`barX/barY` 由 `PinHiddenBar` 缓存，是物理像素还是逻辑像素以现状代码为准）。
- `layoutTools()` 照旧，工具条已有 bottom → top → overlay 三级钳位，出不了界。

### 3. peek 生命周期（原位语义不变）

新增成员 `RECT prevRect`（或等价物），语义：

- `peek(true)`：若未在 peek 中，记 `prevRect = 当前窗口矩形` → 按第 2 节公式挪到线旁 → `show()`。
- `peek(false)` 收回时：
  - 当前**位置仍等于摆位**（图没被拖过；比 x/y 即可，拖拽不改宽高）→ 挪回 `prevRect` 再 `hide()`；
  - 当前**位置不等于摆位**（用户拖走了）→ `prevRect = 当前矩形` 转正为新原位，就地 `hide()`——拖动手感不丢。
- 「隐藏」按钮（`setHidden(true)`）：行为不变，就地藏，`rect` 即原位。
- 「显示」取消隐藏（`setHidden(false)`）：`show()` 当前 rect（上面的逻辑保证它永远是有效原位），行为与现状一致。

### 4. 离开宽限 800ms

现状：`PinHiddenBar` 每 `tickMs = 250` 复核一次，发现鼠标不在「回显图 / 它的两条工具条 / 本窗口」上就立即 `conceal()`。

改为**离开后延 800ms 再收**：

- 新增 `static constexpr UINT peekGraceMs{ 800 }` 与成员 `DWORD leaveAt{ 0 }`（0 = 在场）。
- `onTimerCB` 每跳（250ms 不变）：
  - `isOverPeek()` 为真 → `leaveAt = 0`；
  - 为假且 `leaveAt == 0` → `leaveAt = GetTickCount()`；
  - 为假且 `GetTickCount() - leaveAt >= peekGraceMs` → `conceal()`。
- 中途回到图 / 工具条 / 条上 → 计时清零。实际收回延时 800 ~ 1050ms（含轮询粒度），可接受。

### 5. 不动的部分

- 250ms 轮询本体、`isOverPeek()` 判定范围、`reveal` 的窗口 topmost 提回；
- 颜色轮转、条取最近命中（`barIndexAt`）、两条边各自独立、`sync()` 重建；
- 藏 / 放状态位 `isHidden` 语义、拖到边判定（光标 4px 容差 + `hasDragged`）。

## 验收标准

1. 编译 0 错误；冒烟通过。
2. 条几何：30 长 × 4 厚（±dpi 换算），顶条随之变 30 宽 × 4 厚。
3. hover 左条第 i 条 → 图出现在屏幕左侧、图顶对齐该线顶端、距条 4px；顶条同理（图左对齐该线左端、距条 4px）。
4. 回显图（含工具条）不出工作区（大贴图验证）。
5. 鼠标离开边线且不进入回显图 → 800 ~ 1050ms 内收回；期间回到图 / 工具条 / 边线上则不收回。
6. 回显时不拖动、移开收回 → 图回到藏之前的原位；再次「显示」按钮取消隐藏，位置与隐藏前一致。
7. 回显时把图拖到别处、移开收回 → 图就地隐藏；再 hover 仍从线旁出现；此时「显示」取消隐藏 → 图落在最后拖到的位置。
8. 既有行为回归：颜色序号、条数增删同步、双条窗互不干扰、「隐藏」按钮 toggle 正常。
