#include "pch.h"
#include <algorithm>
#include <cstddef>
#include "Markdown.h"

namespace {
	// Markdown 是个类，成员类型只能起别名，不能写 using Markdown::Block（那是给命名空间的）
	using Block = Markdown::Block;
	using Inline = Markdown::Inline;
	using Kind = Markdown::Kind;

	constexpr float baseFont{ 14.f };
	constexpr float codeFont{ 12.5f };
	constexpr const wchar_t* monoFamily{ L"Consolas" };

	// 表格线 / 表头底色（逻辑像素）
	constexpr uint32_t tableLine{ 0xD8D8DEff };
	constexpr uint32_t tableHeadBg{ 0xF5F6F8ff };
	// 单元格四周的内边距，以及分栏线的粗细
	constexpr float cellPadX{ 8.f };
	constexpr float cellPadY{ 5.f };
	constexpr float cellLine{ 1.f };

	std::wstring trim(const std::wstring& s)
	{
		const size_t b = s.find_first_not_of(L" \t");
		if (b == std::wstring::npos) return L"";
		const size_t e = s.find_last_not_of(L" \t");
		return s.substr(b, e - b + 1);
	}

	bool startsWith(const std::wstring& s, const wchar_t* p)
	{
		return s.rfind(p, 0) == 0;
	}

	// 分割线：至少三个 - / * / _（中间可以有空格），且不能混用
	bool isRule(const std::wstring& s)
	{
		wchar_t mark = 0;
		int n = 0;
		for (const wchar_t c : s) {
			if (c == L' ' || c == L'\t') continue;
			if (c != L'-' && c != L'*' && c != L'_') return false;
			if (mark == 0) mark = c;
			else if (c != mark) return false;
			++n;
		}
		return n >= 3;
	}

	// 表格的分隔行：只有 | - : 和空白，且至少有一个 -
	bool isTableSep(const std::wstring& s)
	{
		if (s.find(L'|') == std::wstring::npos) return false;
		bool dash{ false };
		for (const wchar_t c : s) {
			if (c == L'-') { dash = true; continue; }
			if (c == L'|' || c == L':' || c == L' ' || c == L'\t') continue;
			return false;
		}
		return dash;
	}

	// "| a | b |" -> { "a", "b" }。两端的竖线会各多切出一个空串，只在那一段确实是空的时候丢掉
	//（"|a|b" 这种省了尾竖线的写法，末列不能跟着被吃掉）。\| 是转义，不当分隔符
	std::vector<std::wstring> splitCells(const std::wstring& line)
	{
		std::vector<std::wstring> cells;
		std::wstring cur;
		for (size_t i = 0; i < line.size(); ++i) {
			const wchar_t c = line[i];
			if (c == L'\\' && i + 1 < line.size() && line[i + 1] == L'|') {
				cur.push_back(L'|');
				++i;
				continue;
			}
			if (c == L'|') { cells.push_back(cur); cur.clear(); continue; }
			cur.push_back(c);
		}
		cells.push_back(cur);
		if (!cells.empty() && trim(cells.front()).empty()) cells.erase(cells.begin());
		if (!cells.empty() && trim(cells.back()).empty()) cells.pop_back();
		for (auto& c : cells) c = trim(c);
		return cells;
	}

	// 列表项。命中就把 marker 填上（有序是 "1."，无序留空）并返回 true
	bool listMarker(const std::wstring& line, std::wstring& marker)
	{
		if (line.empty()) return false;
		if (line.size() >= 2 && (line[0] == L'-' || line[0] == L'*' || line[0] == L'+') && line[1] == L' ') {
			marker.clear();
			return true;
		}
		size_t i = 0;
		while (i < line.size() && line[i] >= L'0' && line[i] <= L'9' && i < 9) ++i;
		if (i == 0 || i >= line.size()) return false;
		if (line[i] != L'.' && line[i] != L')') return false;
		if (i + 1 >= line.size() || line[i + 1] != L' ') return false;
		marker = line.substr(0, i + 1);   // 统一成 "N." 的样子，原文用 ")" 也换成 "."
		return true;
	}

	// 这一行是不是"块的开头"（段落遇到它就该收尾）
	bool isBlockStart(const std::wstring& t)
	{
		if (t.empty()) return true;
		if (startsWith(t, L"```") || startsWith(t, L"~~~")) return true;
		if (t[0] == L'#' || t[0] == L'>') return true;
		if (isRule(t)) return true;
		std::wstring m;
		if (listMarker(t, m)) return true;
		return false;
	}

	// 行内解析。** / ` 成对才算，单个落回普通文字；*斜体* 只认星号 —— 下划线在
	// 代码标识符里太常见（foo_bar_baz），当斜体标记会误伤
	std::vector<Inline> parseInline(const std::wstring& src)
	{
		std::vector<Inline> out;
		std::wstring plain;
		auto push = [&out](const std::wstring& t, const bool b, const bool i, const bool mo) {
			if (t.empty()) return;
			if (!out.empty() && out.back().bold == b && out.back().italic == i && out.back().mono == mo) {
				out.back().text += t;
				return;
			}
			out.push_back(Inline{ t, b, i, mo });
		};
		auto flush = [&plain, &push]() { push(plain, false, false, false); plain.clear(); };

		for (size_t i = 0; i < src.size();) {
			const wchar_t c = src[i];
			if (c == L'`') {
				const size_t e = src.find(L'`', i + 1);
				if (e != std::wstring::npos && e > i + 1) {
					flush();
					push(src.substr(i + 1, e - i - 1), false, false, true);
					i = e + 1;
					continue;
				}
			}
			if (c == L'*' && i + 1 < src.size() && src[i + 1] == L'*') {
				const size_t e = src.find(L"**", i + 2);
				if (e != std::wstring::npos && e > i + 2) {
					flush();
					push(src.substr(i + 2, e - i - 2), true, false, false);
					i = e + 2;
					continue;
				}
			}
			if (c == L'*') {
				const size_t e = src.find(L'*', i + 1);
				if (e != std::wstring::npos && e > i + 1) {
					flush();
					push(src.substr(i + 1, e - i - 1), false, true, false);
					i = e + 1;
					continue;
				}
			}
			// 链接只留可见文字；![...] 是图片，也同样只留文字（图不在这里加载）
			const bool image = (c == L'!' && i + 1 < src.size() && src[i + 1] == L'[');
			if (c == L'[' || image) {
				const size_t lb = image ? i + 1 : i;
				const size_t rb = src.find(L']', lb + 1);
				if (rb != std::wstring::npos) {
					const size_t usedTo = (rb + 1 < src.size() && src[rb + 1] == L'(') ? rb + 1 : rb;
					const size_t rp = (usedTo == rb + 1) ? src.find(L')', rb + 2) : std::wstring::npos;
					flush();
					push(src.substr(lb + 1, rb - lb - 1), false, false, false);
					i = (rp == std::wstring::npos) ? rb + 1 : rp + 1;
					continue;
				}
			}
			plain.push_back(c);
			++i;
		}
		flush();
		return out;
	}

	// 行内片段 -> 一整段文字 + 落在它上面的样式区间。区间下标是 UTF-16 单位，
	// 与 DWrite 的 TEXT_RANGE 一致
	struct Flat
	{
		std::wstring text;
		std::vector<Ling::TextRun> runs;
	};
	Flat flatten(const std::vector<Inline>& ins)
	{
		Flat f;
		for (const auto& in : ins) {
			const size_t at = f.text.size();
			f.text += in.text;
			if (in.bold || in.italic || in.mono) {
				f.runs.push_back(Ling::TextRun{ at, in.text.size(), in.bold, in.italic, in.mono });
			}
		}
		return f;
	}

	// 都会用到的那几项：折行 + 不超容器宽 + 行距
	void setupText(Ling::Label* lab)
	{
		lab->setWrap(true);
		lab->setMaxWidthPercent(100.f);
		lab->setFontSize(baseFont);
		lab->setColor(0x333333FF);
		lab->setLineSpacing(Markdown::lineSpacing);
		// ⚠️ 这一条不能省，它管的是"折行宽度算在哪"。
		// yoga 的非 web 默认 flexShrink 是 **0**：一行里前面还有兄弟节点（列表的序号格、
		// 引用的竖条）时，标签的 flex 基准量的是"父容器的可用宽"（不是减掉兄弟之后的余量），
		// 又不会收缩 ⇒ 它自己拿到整行宽、再被摆到兄弟后面，正文就整整顶出右边框 ——
		// 顶出去的正好是兄弟们的宽度（列表 24+4，引用 3+8）。开了收缩，flex 行会把它压回
		// 余量里，最终布局再按压完的宽度重新折行。段落那种独占一行的块本来就用不上收缩
		lab->setFlexShrink(1.f);
	}

	// 只在文字真的变了才动控件：Text::setText 会把 DWrite layout 整个重建，
	// 流式输出每 80ms 走一遍，没变还硬塞一次纯属白干（表面也跟着重画一遍）
	void setTextIfChanged(Ling::Label* lab, const std::wstring& text,
		const std::vector<Ling::TextRun>& runs)
	{
		if (!lab) return;
		if (lab->getText() == text) return;
		lab->setText(text);
		lab->setRuns(runs);
	}

	// 表格的一行。cols 由调用方按"表头那行"的列数定死 —— 模型偶尔会少打一两个竖线，
	// 后面几行就短一截，按 cols 补空格子列才对得齐。
	//
	// 网格线的画法：容器底色就是线的颜色，格子铺不透明的底、彼此之间只留 1 逻辑像素的
	// 缝 —— 缝里透出来的就是线。Ling 的 Node 没有分边框，只有这条路能不靠画布画出网格；
	// 缝用的 margin 不参与 flexShrink（只有格子的宽度会被挤），所以线宽恒为 1
	void buildTableRow(Ling::Node* grid, const Block& block, const size_t r, const int cols)
	{
		auto row = grid->makeChild<Ling::Node>();
		row->setWidthPercent(100.f);
		row->setFlexDirection(Ling::FlexDirection::Row);
		// 交叉轴拉伸：一行里所有格子同高，横向的线才对得齐
		row->setAlignItems(Ling::Align::Stretch);
		if (r > 0) row->setMarginTop(cellLine);
		for (int c = 0; c < cols; ++c) {
			const std::wstring txt = c < static_cast<int>(block.cells[r].size())
				? block.cells[r][c] : std::wstring{};
			auto cell = row->makeChild<Ling::Label>();
			setupText(cell);
			// 等宽。合计比容器宽多出 (cols-1) 个像素（那几条缝），交给 flexShrink 摊掉 ——
			// setupText 已经开了收缩，压完的宽度会在最终布局里重新折一次行
			cell->setWidthPercent(100.f / static_cast<float>(cols));
			cell->setPadding(cellPadX, cellPadY, cellPadX, cellPadY);
			cell->setBg(r == 0 ? tableHeadBg : 0xFFFFFFFF);
			// 分栏缝：只给第一栏之后的格子加，整行右边缘就不会多出一道
			if (c > 0) cell->setMarginLeft(cellLine);
			// 对齐要落在**主轴**上。Label 是"外层容器 + 内层文字"的复合件，拿交叉轴的
			// alignItems 去摆内层文字会让 yoga 量它时不给可用宽度（折行失效 ——
			// 见 WinAiChat::addItem 里那段注释），所以改成 Row + justifyContent
			cell->setFlexDirection(Ling::FlexDirection::Row);
			const int a = c < static_cast<int>(block.aligns.size()) ? block.aligns[c] : 0;
			cell->setJustifyContent(a == 1 ? Ling::Justify::Center
				: (a == 2 ? Ling::Justify::End : Ling::Justify::Start));
			if (r == 0) cell->setColor(0x1F1F1FFF);
			cell->setText(txt);
			if (r == 0 && !txt.empty()) {
				cell->setRuns({ Ling::TextRun{ 0, txt.size(), true, false, false } });
			}
		}
	}

	// 一行表格里的格子（按创建顺序）。挑 Label 而不是按下标取 children ——
	// 以后要往行里插别的东西（线、空白格）时，这里不会跟着错位
	std::vector<Ling::Label*> tableRowCells(Ling::Node* row)
	{
		std::vector<Ling::Label*> out;
		if (!row) return out;
		for (auto& ch : row->children) {
			if (auto lab = dynamic_cast<Ling::Label*>(ch.get())) out.push_back(lab);
		}
		return out;
	}
}

std::vector<Block> Markdown::parse(const std::wstring& src)
{
	std::vector<std::wstring> lines;
	{
		std::wstring cur;
		for (const wchar_t c : src) {
			if (c == L'\r') continue;
			if (c == L'\n') { lines.push_back(cur); cur.clear(); }
			else cur.push_back(c);
		}
		lines.push_back(cur);
	}

	std::vector<Block> out;
	for (size_t i = 0; i < lines.size();) {
		const std::wstring line = trim(lines[i]);
		if (line.empty()) { ++i; continue; }

		// 围栏代码块。收尾栅栏只认同一种字符，没等到就吃到末尾（回答没写完时就是这样）
		if (startsWith(line, L"```") || startsWith(line, L"~~~")) {
			const wchar_t fence = line[0];
			const std::wstring close(3, fence);
			++i;
			std::wstring code;
			while (i < lines.size()) {
				if (startsWith(trim(lines[i]), close.c_str())) { ++i; break; }
				if (!code.empty()) code += L'\n';
				code += lines[i];
				++i;
			}
			Block b;
			b.kind = Kind::Code;
			b.text = code;
			out.push_back(std::move(b));
			continue;
		}

		if (isRule(line)) {
			Block b;
			b.kind = Kind::Rule;
			out.push_back(std::move(b));
			++i;
			continue;
		}

		if (line[0] == L'#') {
			size_t n = 0;
			while (n < line.size() && n < 6 && line[n] == L'#') ++n;
			if (n < line.size() && line[n] == L' ') {
				Block b;
				b.kind = Kind::Heading;
				b.level = static_cast<int>(n);
				b.inlines = parseInline(trim(line.substr(n + 1)));
				out.push_back(std::move(b));
				++i;
				continue;
			}
		}

		// 引用：连续的 ">" 行并成一段
		if (line[0] == L'>') {
			std::wstring text;
			while (i < lines.size() && startsWith(trim(lines[i]), L">")) {
				if (!text.empty()) text += L' ';
				text += trim(trim(lines[i]).substr(1));
				++i;
			}
			Block b;
			b.kind = Kind::Quote;
			b.inlines = parseInline(text);
			out.push_back(std::move(b));
			continue;
		}

		std::wstring marker;
		if (listMarker(line, marker)) {
			Block b;
			b.kind = Kind::List;
			b.ordered = !marker.empty();
			b.marker = marker;
			// 序号那一格的下标按"标记 + 一个空格"算，"1. " 是 3 个字符
			const size_t skip = line.find(L' ');
			b.inlines = parseInline(skip == std::wstring::npos ? L"" : line.substr(skip + 1));
			out.push_back(std::move(b));
			++i;
			continue;
		}

		// 表格：本行有 | 且下一行是分隔行。列宽与对齐由分隔行定（:--- 左 / :---: 中 / ---: 右），
		// 之后连续含 | 的行都是这一张表的记录行
		if (line.find(L'|') != std::wstring::npos && i + 1 < lines.size()
			&& isTableSep(trim(lines[i + 1]))) {
			Block b;
			b.kind = Kind::Table;
			// 第 0 行是表头。行内标记（**粗体**、`代码`）在解析时就拍平成字面文字 ——
			// 一格一格地画行内样式太碎，换来的是"格子里不再出现裸的星号"
			auto pushRow = [&b](const std::wstring& src) {
				std::vector<std::wstring> row;
				for (auto& c : splitCells(src)) row.push_back(flatten(parseInline(c)).text);
				b.cells.push_back(std::move(row));
			};
			pushRow(line);
			for (auto& s : splitCells(trim(lines[i + 1]))) {
				const bool l = !s.empty() && s.front() == L':';
				const bool r = !s.empty() && s.back() == L':';
				b.aligns.push_back(l && r ? 1 : (r ? 2 : 0));
			}
			i += 2;
			while (i < lines.size() && trim(lines[i]).find(L'|') != std::wstring::npos) {
				pushRow(trim(lines[i]));
				++i;
			}
			out.push_back(std::move(b));
			continue;
		}

		// 段落：连续的普通行并成一段（markdown 的软换行按空格接起来）。
		// 第一行无条件吃下 —— 上面几个分支都没命中时总得往前走，不然就是死循环
		{
			std::wstring text;
			while (i < lines.size()) {
				const std::wstring t = trim(lines[i]);
				if (t.empty()) break;
				if (!text.empty() && (isBlockStart(t) || t.find(L'|') != std::wstring::npos)) break;
				if (!text.empty()) text += L' ';
				text += t;
				++i;
			}
			Block b;
			b.kind = Kind::Paragraph;
			b.inlines = parseInline(text);
			out.push_back(std::move(b));
		}
	}
	return out;
}

std::wstring Markdown::tag(const Block& block)
{
	std::wstring t{ L"md:" };
	t += std::to_wstring(static_cast<int>(block.kind));
	t += L':';
	t += std::to_wstring(block.level);
	t += block.ordered ? L":o" : L":u";
	// 表格：列数与每列的对齐属于骨架（列数变了得整块重建）。行数**不**算 —— 流式输出时
	// 表格是一行行长出来的，refresh 会就地补行，带上行数就会每来一行重建一次、正文一直闪。
	// 列数以表头那一行为准
	if (block.kind == Kind::Table) {
		const size_t cols = block.cells.empty() ? 0 : block.cells.front().size();
		t += L":";
		t += std::to_wstring(cols);
		t += L":";
		for (size_t c = 0; c < cols; ++c) {
			const int a = c < block.aligns.size() ? block.aligns[c] : 0;
			t += static_cast<wchar_t>(L'0' + std::clamp(a, 0, 9));
		}
	}
	return t;
}

// 就地刷新。前提是 node 的 id 与这个块的身份对得上（render 盖的章）。
// 只改文字 / 行内样式 / 列表序号 —— 字体、颜色、边距这些"块级样式"只由块的种类决定，
// 同类块之间不会变，不必也不该在这里动
bool Markdown::refresh(const Block& block, Ling::Node* node)
{
	if (!node || node->id != tag(block)) return false;
	switch (block.kind) {
	case Kind::Rule:
		// 一条横线，什么状态都没有
		return true;
	case Kind::Code:
	case Kind::Heading:
	case Kind::Paragraph: {
		auto lab = dynamic_cast<Ling::Label*>(node);
		if (!lab) return false;
		if (block.kind == Kind::Code) {
			setTextIfChanged(lab, block.text, {});
		}
		else {
			const auto f = flatten(block.inlines);
			// 标题整段加粗（与 render 里一致：标题里再套行内样式不做区间合并）
			const std::vector<Ling::TextRun> runs = block.kind == Kind::Heading
				? std::vector<Ling::TextRun>{ Ling::TextRun{ 0, f.text.size(), true, false, false } }
				: f.runs;
			setTextIfChanged(lab, f.text, runs);
		}
		return true;
	}
	case Kind::Quote: {
		// Row[竖条, 文字]
		if (node->children.size() != 2) return false;
		auto lab = dynamic_cast<Ling::Label*>(node->children[1].get());
		if (!lab) return false;
		const auto f = flatten(block.inlines);
		setTextIfChanged(lab, f.text, f.runs);
		return true;
	}
	case Kind::List: {
		// Row[序号格, 文字]。序号格那一枚的字（"1." / 圆点）也要跟着更
		if (node->children.size() != 2) return false;
		auto mk = dynamic_cast<Ling::Label*>(node->children[0].get());
		auto lab = dynamic_cast<Ling::Label*>(node->children[1].get());
		if (!mk || !lab) return false;
		setTextIfChanged(mk, block.ordered ? block.marker : L"\u2022", {});
		const auto f = flatten(block.inlines);
		setTextIfChanged(lab, f.text, f.runs);
		return true;
	}
	case Kind::Table: {
		// 列数由 tag 保证一致（变了就整块重建）。行数可以变，就地补行 / 改字 ——
		// 重建要重开一批 composition 绘制表面，流式输出时正文会一直闪
		if (block.cells.empty()) return false;
		const int cols = static_cast<int>(block.cells.front().size());
		if (cols <= 0) return false;
		while (node->children.size() > block.cells.size()) {
			node->removeChild(node->children.back().get());
		}
		for (size_t r = 0; r < block.cells.size(); ++r) {
			if (r >= node->children.size()) {
				buildTableRow(node, block, r, cols);
				continue;
			}
			const auto labs = tableRowCells(node->children[r].get());
			// 这一行的格子数与列数对不上（模型少打了竖线）—— 就地改字补不了，交给调用方重建
			if (static_cast<int>(labs.size()) != cols) return false;
			const int n = static_cast<int>(block.cells[r].size());
			for (int c = 0; c < cols; ++c) {
				const std::wstring txt = c < n ? block.cells[r][c] : std::wstring{};
				const std::vector<Ling::TextRun> runs = (r == 0 && !txt.empty())
					? std::vector<Ling::TextRun>{ Ling::TextRun{ 0, txt.size(), true, false, false } }
					: std::vector<Ling::TextRun>{};
				setTextIfChanged(labs[c], txt, runs);
			}
		}
		return true;
	}
	}
	return false;
}

void Markdown::render(const Block& block, Ling::Node* parent, const bool first)
{
	const float top = first ? 0.f : 6.f;
	// 这一块渲染出来的是哪个顶层控件：switch 里每个分支最后一句都是 makeChild，
	// 所以"多出来的那一个"就是它。段落文案为空时可能一个都不建
	const size_t before = parent->children.size();
	switch (block.kind) {
	case Kind::Rule: {
		auto hr = parent->makeChild<Ling::Node>();
		hr->setWidthPercent(100.f);
		hr->setHeight(1.f);
		hr->setBg(0xDCDCE0FF);
		hr->setMarginTop(10.f);
		hr->setMarginBottom(10.f);
		break;
	}
	case Kind::Code: {
		auto lab = parent->makeChild<Ling::Label>();
		setupText(lab);
		lab->setFontFamily(monoFamily);
		lab->setFontSize(codeFont);
		lab->setColor(0x2F2F35FF);
		lab->setMarginTop(top);
		lab->setBg(0xE6E6ECFF);
		lab->setPadding(8.f, 6.f, 8.f, 6.f);
		lab->setBorderRadius(6.f);
		lab->setText(block.text);
		break;
	}
	case Kind::Table: {
		if (block.cells.empty()) break;
		const int cols = static_cast<int>(block.cells.front().size());
		if (cols <= 0) break;
		auto grid = parent->makeChild<Ling::Node>();
		grid->setWidthPercent(100.f);
		grid->setFlexDirection(Ling::FlexDirection::Column);
		grid->setMarginTop(first ? 0.f : 8.f);
		// 容器底色 = 线的颜色：格子之间那 1 像素的缝、格子与外框之间的边距都透出它。
		// 外框由容器自己描，格子不再各描一圈，边上就不会叠成两条
		grid->setBg(tableLine);
		grid->setBorder(cellLine, tableLine);
		grid->setBorderRadius(4.f);
		for (size_t r = 0; r < block.cells.size(); ++r) buildTableRow(grid, block, r, cols);
		break;
	}
	case Kind::Heading: {
		static const float hs[7]{ 0.f, 18.f, 16.5f, 15.5f, 14.5f, 14.f, 14.f };
		const auto f = flatten(block.inlines);
		auto lab = parent->makeChild<Ling::Label>();
		setupText(lab);
		lab->setFontSize(hs[std::clamp(block.level, 1, 6)]);
		lab->setColor(0x1F1F1FFF);
		lab->setMarginTop(first ? 0.f : 8.f);
		lab->setText(f.text);
		// 标题整段加粗。标题里再套行内样式的概率很低，而叠起来要做区间合并，不值当
		lab->setRuns({ Ling::TextRun{ 0, f.text.size(), true, false, false } });
		break;
	}
	case Kind::Quote: {
		auto row = parent->makeChild<Ling::Node>();
		row->setWidthPercent(100.f);
		row->setFlexDirection(Ling::FlexDirection::Row);
		// 竖条靠 Stretch 撑到与正文同高，所以这里不能用默认的 FlexStart
		row->setAlignItems(Ling::Align::Stretch);
		row->setMarginTop(top);
		auto bar = row->makeChild<Ling::Node>();
		bar->setWidth(3.f);
		bar->setBg(0xC8C8D0FF);
		bar->setBorderRadius(2.f);
		const auto f = flatten(block.inlines);
		auto lab = row->makeChild<Ling::Label>();
		setupText(lab);
		lab->setFlexGrow(1.f);
		lab->setColor(0x5F6368FF);
		lab->setMarginLeft(8.f);
		lab->setText(f.text);
		lab->setRuns(f.runs);
		break;
	}
	case Kind::List: {
		auto row = parent->makeChild<Ling::Node>();
		row->setWidthPercent(100.f);
		row->setFlexDirection(Ling::FlexDirection::Row);
		row->setAlignItems(Ling::Align::FlexStart);
		row->setMarginTop(first ? 0.f : 3.f);
		auto mk = row->makeChild<Ling::Label>();
		// 序号那一格定宽并右对齐，正文才能与标记的右边对齐（悬挂缩进）
		mk->setWidth(24.f);
		mk->setAlignItems(Ling::Align::FlexEnd);
		mk->setFontSize(baseFont);
		mk->setColor(0x333333FF);
		mk->setMarginRight(4.f);
		mk->setText(block.ordered ? block.marker : L"\u2022");
		const auto f = flatten(block.inlines);
		auto lab = row->makeChild<Ling::Label>();
		setupText(lab);
		lab->setFlexGrow(1.f);
		lab->setText(f.text);
		lab->setRuns(f.runs);
		break;
	}
	case Kind::Paragraph:
	default: {
		const auto f = flatten(block.inlines);
		if (f.text.empty()) break;
		auto lab = parent->makeChild<Ling::Label>();
		setupText(lab);
		lab->setMarginTop(top);
		lab->setText(f.text);
		lab->setRuns(f.runs);
		break;
	}
	}
	// 给这一块渲染出来的顶层控件盖个"身份章"：流式输出下一遍重画时，
	// 同类的块就是靠它认出来的（见 refresh）
	if (parent->children.size() > before) parent->children.back()->setId(tag(block));
}
