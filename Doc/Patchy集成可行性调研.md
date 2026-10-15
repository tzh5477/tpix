# Patchy 集成到 tpix 的可行性调研

> 调研性质：仅调研，不含代码开发。
> 日期：2026-10-13

## 0. 结论（先讲重点）

**不建议将 Patchy 以"代码层面 / 库嵌入"的方式集成进 tpix。** 二者在四个维度存在根本冲突：图像模型范式、UI 框架、构建体系、分发形态。

可行的、风险最低的集成是**进程级协作**：tpix 通过 Patchy 的命令行 / 无头脚本 / MCP 接口，把"高级编辑"与"PSD 读写"这类 tpix 不擅长的事交给 Patchy 完成，再以文件回传。其中**单向 PSD 导入（用 Patchy 无头栅格化为位图后再交由 tpix 标注）** 与 tpix 现有的"底图 + 标注"模型天然契合，是最优切入点。

---

## 1. Patchy 是什么

- **定位**：开源（MIT）的 Photoshop 平替，专注 **PSD / PSB 高精度双向兼容**；文字、矢量、蒙版、图层样式、智能对象、智能滤镜可往返编辑。
- **平台**：Windows（主平台）、macOS（Apple Silicon）、Linux（Flatpak），以及 **WebAssembly 浏览器版**。
- **技术栈**：**C++ + Qt 6**（qt6-base / qt6-declarative / qt6-imageformats / qt6-svg），**不使用 GPU（纯 CPU 渲染）**。AUR 依赖另含 kimageformats、可选 libheif（HEIC）。
- **规模与活跃度**：约 560 Star、33 Fork、1450+ commits；最新 1.08（2026-10-10），非常活跃。
- **许可**：**MIT** —— 商用 / 集成无法律障碍。

### 1.1 Patchy 对外暴露的"集成面"（关键资产）

来自官方 `docs/features.md`（已核实）：

1. **JavaScript 脚本引擎**：内置 Script Manager，文档化 API 覆盖 documents / layers / text / selections / pixels / filters / 文件选择 / 批处理。
2. **命令行自动化**：`--run-script <脚本> [参数]`，并可加 `--headless` **无显示运行**（服务器 / CI 场景）。
3. **原生 MCP 连接器**：本地 AI 智能体可"绘制、预览、改图层、保存可编辑工程"。
4. **8BF 滤镜插件**（Windows 端）。

- 文件格式：PSD、PSB、SVG（可编辑形状层）、部分 Affinity 导入、PDF（导入限桌面端）、动图 WebP/GIF、DDS(BC7/BC4/BC5)、CMYK 图层。

> 注意：存在一个同名但无关的项目 `copyleftdev/patchy`（Go 写的 MCP 服务），与本次所调研的 SethRobinson/Patchy 无关，勿混淆。

---

## 2. tpix 现状与定位（来自 AGENT.md / ReadMe.md）

- **定位**：Windows 开源截图贴图工具，对标 PixPin。
- **技术栈**：C++20 + **Win32 + Direct2D + Windows Composition + 自研 GUI 框架 Ling**。**明确不使用 Qt / MFC / 任何第三方 UI 库。**
- **图像模型**：**单张底图（ID2D1Bitmap1）+ 矢量标注层（Shape*）**。Canvas/Shape 已抽到画布宿主，但本质仍是"截图 + 标注"，**不是分层光栅编辑器**。
- **分发哲学**：**单个 ~1MB exe，免安装，除 OCR 插件外不依赖任何 DLL。**
- **外部依赖**：Ling.lib（自研可控上游）、gifski.lib、onnxruntime（表格识别）。
- **构建**：MSVC vcxproj（v145/v143），手工构建链敏感。

---

## 3. 集成方案逐条评估

### 方案 A：在 tpix 里嵌入 Patchy 的 UI / 窗体 —— ❌ 不可行
- Patchy 用 Qt6，tpix 用自研 Ling/Win32，两套 UI 框架无法互嵌窗口；且 tpix 明确排斥 Qt。
- 结论：架构上排除。

### 方案 B：把 Patchy 的"核心引擎"编译成静态库供 tpix 调用 —— ⚠️ 代价极高，不推荐
- Patchy 的 `src/` 虽分 app/core/ui/render/psd/formats，但核心逻辑与 Qt 应用层 / 场景图的耦合程度未知；把"无头引擎"剥离出来是重量级移植。
- 渲染后端不一致：Patchy 纯 CPU / Qt 场景图；tpix 是 D2D。像素需从 Patchy 缓冲桥接到 `ID2D1Bitmap1`。
- 即便剥离成功，tpix 也需背负 Patchy 引擎体积，与"1MB 单 exe"定位冲突。
- 实质接近"重写"，不是"集成"。

### 方案 C：进程级"用 Patchy 编辑" —— ✅ 可行、低风险（推荐主线）
- tpix 增加"用 Patchy 编辑"入口：把当前图（PNG/PSD）交给已安装 / 随附的 Patchy 进程，编辑保存后回读。
- 优点：零代码耦合，保留双方架构；直接获得 PSD / 高级编辑能力。
- 缺点：需 Patchy 在场（安装或随附，与轻量定位冲突）；双进程；文件往返。

### 方案 D：无头脚本 / CLI 自动化，让 tpix 驱动 Patchy 做特定任务 —— ✅ 可行（最契合 tpix 的"工具"气质）
- 用 `patchy --headless --run-script <js> -- <args>` 完成 tpix 当前缺的能力，例如：
  - **PSD → PNG 栅格化**（单向导入：tpix 把 PSD 交给 Patchy 压平为位图，再回到"底图 + 标注"流程——与 tpix 模型完美契合）；
  - 应用某 8BF 滤镜 / 批量格式转换。
- 优点：无 UI 纠缠、可脚本化、可无头；比方案 C 更"嵌入式"。
- 缺点：仍需 Patchy 二进制在场；以文件 / stdin 做进程间通信；需逐一确认 JS API 是否覆盖所需操作。

### 方案 E：MCP 连接器（AI 驱动）—— ⚠️ 投机，优先级低
- tpix 已有 AI 翻译 / 对话（D4）。理论上可经 Patchy 的 MCP 让 AI 做分层编辑。但这依赖 Patchy 常驻，本质是 AI 智能体特性，而非代码集成；与 tpix 离线、轻量定位不符。

### 方案 F：只取 PSD 能力，用"专注型 PSD 库"替代 —— ✅ 若仅需 PSD，ROI 更高
- 若 tpix 的真实诉求只是"读 / 写 PSD"，引入 Patchy 整引擎是杀鸡用牛刀。一个轻量 PSD 解析 / 生成库（或仅抽取 Patchy 的 `src/psd` 并解耦）更契合 tpix 极简哲学。

---

## 4. 关键不匹配点（决策依据）

| 维度 | Patchy | tpix | 冲突 |
|---|---|---|---|
| 图像模型 | 分层光栅编辑器（PSD/PSB） | 单底图 + 矢量标注 | 范式不同，Patchy 核心能力难以映射到 tpix |
| UI 框架 | Qt 6 | 自研 Ling / Win32 / D2D | 无法互嵌 |
| 构建 | CMake + Qt + 跨平台 | MSVC vcxproj + Ling | 构建体系不兼容 |
| 分发形态 | 大型 Qt 应用 | 1MB 单 exe、零 DLL | 随附 Patchy 直接违背 tpix 定位 |
| 平台 | 跨平台（含浏览器） | 仅 Windows | tpix 只能在 Windows 端用 Patchy |

---

## 5. 建议

1. **不做代码级 / 库级嵌入**（排除 A、B）。
2. 若目标是"让 tpix 用户能编辑 PSD / 做高级编辑"：走 **方案 D（无头脚本，PSD→PNG 单向导入）作为最小可行切入点**，其次 **方案 C（编辑后回读）**。
3. 若目标只是"读写 PSD 工程"，评估 **方案 F（专注型 PSD 库）** 的 ROI 优于拖入 Patchy 整引擎。
4. MCP（方案 E）仅作为"AI 辅助分层编辑"远期设想，不作为集成主线。

---

## 6. 待确认 / 需进一步核实

- **Patchy 的 JS API 具体能否覆盖"把 PSD 栅格化为指定尺寸的 PNG"**（方案 D 前提）——需读 `docs/` 脚本指南或 `src/` 脚本绑定；本次因无法直连原始文件未逐行核实，但官方特性页已声明支持 pixels / file / batch。
- **Patchy"核心引擎"与 Qt 的解耦程度**——决定方案 B 是否可能；当前仅能从模块划分推断，未读源码。
- **分发策略**：若采用 C/D，是要求用户自装 Patchy、还是随附（破坏轻量定位）——需产品决策。
- **无头模式在 Windows 下的可用性**：特性页以"服务器 / CI"举例，桌面 Windows 实测未覆盖。

---

## 7. 信息来源与可信度

- **已核实（高可信）**：GitHub API 仓库元数据、语言构成、AUR 依赖清单、官方 `docs/features.md` 经搜索引擎返回的原文（Qt 技术栈、`--headless`/`--run-script`、MCP、格式支持）。
- **推断（中可信）**：核心与 Qt 耦合程度、渲染后端差异——基于 `src/` 模块划分 + 官方 "Built with C++ and Qt / No GPU" 声明。
- **未核实（受环境限制）**：Patchy 源码逐行细节、JS API 完整签名——本环境无法直连 raw 文件（curl 出网被拦截，WebFetch 无法解码 base64 blob）。
