#pragma once
#include <string>
#include <vector>
#include <include/Ling.h>

// 对话里用到的极简 markdown。刻意只覆盖模型回答里真会出现的写法：
//   块级 —— 标题（# ~ ######）、段落、围栏代码块、有序/无序列表、引用、分割线；
//          表格不做对齐，整块按等宽原样显示
//   行内 —— **粗体**、*斜体*、`行内代码`；链接只留可见文字（不可点、不画下划线）
// 不做的：嵌套（列表套列表、引用套引用）、HTML、脚注、任务列表、行内图片。
// 解析与渲染分成两步：parse 只切块，render 才建控件 —— 调用方要重建整条消息时
// （流式输出每来一段就要重画一次）只需要重跑这两步，不必自己维护控件。
class Markdown
{
public:
	// 对话区正文的行距倍数（相对字体自带行高，1 = 单倍行距）。用户条与回答条共用同一个
	// 值 —— 两条路的文字都是"正文样式"，各写各的迟早会飘
	static constexpr float lineSpacing{ 1.25f };

	// 行内的一段：一段同样式文字
	struct Inline
	{
		std::wstring text;
		bool bold{ false };
		bool italic{ false };
		bool mono{ false };
	};
	enum class Kind { Paragraph, Heading, Code, List, Quote, Rule };
	struct Block
	{
		Kind kind{ Kind::Paragraph };
		int level{ 0 };          // 标题级别 1~6
		bool ordered{ false };   // 列表：有序 / 无序
		std::wstring marker;     // 列表序号（"1."）；无序的圆点在渲染时才补
		std::wstring text;       // 代码块原文（表格也走这条路）
		bool table{ false };     // 是表格：同样式等宽，但不给底色
		std::vector<Inline> inlines;
	};

	// 把原文切成块。纯文本也能走 —— 结果就是一段一段的 Paragraph
	static std::vector<Block> parse(const std::wstring& src);
	// 把一个块渲染成 parent 的子节点。first 表示它是整条消息的第一块（不加上边距，
	// 让气泡自己的 padding 去管那点留白）
	static void render(const Block& block, Ling::Node* parent, bool first);
	// 块的"身份"：同类 / 同级 / 同有序性 / 同表格的块，渲染出来的顶层控件骨架是同一副，
	// 可以复用。render 会把它盖在节点的 id 上
	static std::wstring tag(const Block& block);
	// 就地刷新一个已经渲染好的块：只改文字与行内样式，不碰节点本身。
	// node 不是 tag(block) 对应的那个顶层控件时返回 false，调用方应当把它整段重建。
	//
	// 存在的唯一理由是**流式输出**：每来一小段就 removeAllChildren + 重建的话，
	// 每遍都要新开一批 composition 绘制表面，正文就会一直闪。同类块就地改字即可
	static bool refresh(const Block& block, Ling::Node* node);
};
