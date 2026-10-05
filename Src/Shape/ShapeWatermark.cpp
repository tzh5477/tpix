#include "pch.h"
#include <cwchar>
#include "Canvas.h"
#include "Tool/ToolSub.h"
#include "Tool/ToolMain.h"
#include "History.h"
#include "ShapeWatermark.h"

namespace {
	// 时间占位符的零件。名字沿用 C# 那套自定义格式串（yyyy / MM / dd / HH / mm / ss），
	// 认得的人多；大小写有意思（MM 是月、mm 是分），所以逐个精确匹配而不是统一大小写。
	// 第二列是喂给 wcsftime 的格式
	struct TimeToken {
		const wchar_t* name;
		const wchar_t* fmt;
	};
	const TimeToken timeTokens[]{
		{ L"{yyyy}", L"%Y" }, { L"{yy}",   L"%y" }, { L"{MM}", L"%m" },
		{ L"{dd}",   L"%d" }, { L"{HH}",   L"%H" }, { L"{mm}", L"%M" }, { L"{ss}", L"%S" },
	};
}

const std::vector<std::wstring>& ShapeWatermark::timeFormats()
{
	static const std::vector<std::wstring> list{
		L"{yyyy}-{MM}-{dd}",
		L"{yyyy}/{MM}/{dd}",
		L"{yyyy}年{MM}月{dd}日",
		L"{yyyy}-{MM}-{dd} {HH}:{mm}",
		L"{yyyy}-{MM}-{dd} {HH}:{mm}:{ss}",
		L"{HH}:{mm}",
		L"{HH}:{mm}:{ss}",
		L"{yyyy}{MM}{dd}",
	};
	return list;
}

std::wstring ShapeWatermark::expandTime(const std::wstring& text, std::time_t stamp)
{
	// 连一个花括号都没有就没什么可换的，直接返回原串 —— 也省掉下面那一趟扫描
	if (text.find(L'{') == std::wstring::npos) return text;
	std::tm tm{};
	localtime_s(&tm, &stamp);
	std::wstring out;
	out.reserve(text.size() + 16);
	size_t i{ 0 };
	while (i < text.size()) {
		if (text[i] == L'{') {
			bool matched{ false };
			for (auto& t : timeTokens) {
				auto n = wcslen(t.name);
				// compare 自己会把越界的那段截掉，末尾只剩半个花括号时不会读到界外
				if (text.compare(i, n, t.name) == 0) {
					wchar_t buf[16]{};
					out.append(buf, wcsftime(buf, 16, t.fmt, &tm));
					i += n;
					matched = true;
					break;
				}
			}
			if (matched) continue;
		}
		out.push_back(text[i]);
		++i;
	}
	return out;
}

ShapeWatermark::ShapeWatermark(Canvas* win) : ShapeBase(win)
{
}

ShapeWatermark::~ShapeWatermark()
{
}

void ShapeWatermark::setCursor()
{
	// 与序号一致：水印是"点一下就落"的元素，用系统箭头就够了
	SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

void ShapeWatermark::mouseMove(const float x, const float y)
{
	// 水印是"铺满整张图的一层"，命中区就是整张底图。只有正在用水印工具时才认它 ——
	// 否则选完水印再切到矩形，鼠标一动悬停就落在水印上：WinPin::onDown 认为"点在已有元素上"
	// 转去选中它，矩形根本建不出来；调颜色 / 字号也全作用到它身上。
	// 反过来，水印工具下它又必须吃悬停，否则这一层谁也没法选中、Delete 删不掉
	if (win->getCurToolId() != L"watermark") {
		hoverDraggerIndex = -1;
		return;
	}
	hoverDraggerIndex = 0;
}

bool ShapeWatermark::makeLayout()
{
	layout.Reset();
	brush.Reset();
	textW = 0.f;
	textH = 0.f;
	if (!win->getToolSub()) return false;
	auto sub = win->getToolSub();
	auto raw = sub->watermarkText;
	if (raw.empty()) return false;
	// 时间占位符只在"文字内容变了"的那一刻求值，之后的重画沿用同一个时刻。
	// 每次 paint 都取当前时间的话，带秒的格式会在每次重画时跳一个数，
	// 屏幕上看到的和导出那份还可能差一秒 —— 水印上的时间该是"这一笔写上去的时候"
	if (raw != lastText) {
		lastText = raw;
		stamp = std::time(nullptr);
	}
	auto text = expandTime(raw, stamp);
	// 取水印自己的字号，不要 getSliderVal：那个返回的是"当前工具"的滑块值 ——
	// 切到文本工具后它就是文本字号，水印会跟着一起变大（作者实测的正是这个）。
	// getWatermarkFontSizePx 内部已乘 dpi，给的就是物理像素，这里不能再乘一次，
	// 否则 150% 缩放下 24 号被算成 54，字被放大、平铺步长跟着变大，看着就是"稀得看不见字"
	auto fontSize = sub->getWatermarkFontSizePx();
	layout = Ling::D2D::makeTextLayout(text, fontSize);
	if (!layout) return false;
	// 字体名要在建完 layout 之后单独设：Ling 的 makeTextLayout 用的是系统字体集合，
	// 换族名在同一集合内就能换（与 ShapeText 同一条路子）
	auto& family = sub->getWatermarkFontFamily();
	if (!family.empty()) layout->SetFontFamilyName(family.c_str(), { 0, INT_MAX });
	DWRITE_TEXT_METRICS m{};
	if (FAILED(layout->GetMetrics(&m))) return false;
	textW = m.width;
	textH = m.height;
	// 不透明度是滑块给的 0~1，颜色在工具条上。alpha 取"档位"这一个值就够了 ——
	// 色板里每种颜色自己的 alpha 恒是 0xFF，乘不乘没区别
	auto alpha = sub->getWatermarkOpacity();
	// 颜色取水印自己那一份（getWatermarkColor），不能借 getSelectedColor ——
	// 那个是"当前工具"的选中色，切到矩形把填充色改成绿色后水印每帧都会跟着变绿。
	// 与上面字号 / 字体 / 不透明度同一条路子：水印的样式只认 watermark 这一组。
	// 解码仍走同一条路径（Ling::Color(...).getD2DColor()）—— 原来这里手写移位把
	// 0xRRGGBBAA 拆错位（每一路都少移 8 位，R 取成 G、B 取成 A），调色板的 alpha
	// 又恒是 0xFF，于是任何颜色都带满蓝：红色 0xCF1322FF 被解成 (19,34,255) —— 画出来就是蓝的
	auto c = sub->getWatermarkColor();
	c.a = alpha;
	if (FAILED(Ling::D2D::get()->deviceContext->CreateSolidColorBrush(c, brush.GetAddressOf()))) return false;
	return true;
}

void ShapeWatermark::drawOne(ID2D1DeviceContext* ctx, float x, float y, float rotation,
	const D2D1_MATRIX_3X2_F& outer)
{
	// 外层变换（屏幕上是缩放、导出时是单位阵）要左乘保住，否则 scale != 1 时
	// 水印不跟随缩放；平移、旋转、居中三步排在内层
	auto m = outer * D2D1::Matrix3x2F::Translation(x, y) * D2D1::Matrix3x2F::Rotation(rotation);
	ctx->SetTransform(m);
	ctx->DrawTextLayout({ -textW / 2.f, -textH / 2.f }, layout.Get(), brush.Get());
}

D2D1_POINT_2F ShapeWatermark::anchorPos(WmPos pos, float imgW, float imgH) const
{
	// 边距按文字自身高度给：小字自动贴得近，大字也不会顶到（或压住）图的边线
	auto gap = std::max(textH * 0.5f, 8.f);
	auto l{ gap + textW / 2.f }, t{ gap + textH / 2.f };
	auto r{ imgW - gap - textW / 2.f }, b{ imgH - gap - textH / 2.f };
	switch (pos)
	{
	case WmPos::RightBottom:  return D2D1::Point2F(r, b);
	case WmPos::LeftBottom:   return D2D1::Point2F(l, b);
	case WmPos::RightTop:     return D2D1::Point2F(r, t);
	case WmPos::LeftTop:      return D2D1::Point2F(l, t);
	case WmPos::TopCenter:    return D2D1::Point2F(imgW / 2.f, t);
	case WmPos::BottomCenter: return D2D1::Point2F(imgW / 2.f, b);
	default:                  return D2D1::Point2F(imgW / 2.f, imgH / 2.f);
	}
}

void ShapeWatermark::paint(ID2D1DeviceContext* ctx)
{
	if (!makeLayout()) return;
	// 直接问底图要像素尺寸（不是 GetSize —— 那个按位图自身 dpi 折过）。平铺范围就是整张底图，
	// 四角 / 居中的落点也要按它算，拿折过的尺寸会让水印整体偏到图中间去
	auto sz = win->getImgSize();
	if (sz.width == 0 || sz.height == 0) return;
	auto sub = win->getToolSub();
	auto pos = static_cast<WmPos>(sub->getWatermarkPos());
	// 屏幕绘制时外层是缩放变换、导出时是单位阵，进来是什么出去还是什么，
	// 不能自己设回 Scale —— 导出图会被放大 scale 倍
	D2D1_MATRIX_3X2_F prev{};
	ctx->GetTransform(&prev);
	if (pos == WmPos::Tile) {
		auto rotation = sub->getWatermarkRotation();
		// 平铺：网格建在"文字自己的坐标系"里 —— u 沿文字方向、v 垂直文字方向。
		// 这样留出的空隙是顺着文字斜的（看着就是正常的行距），整张图处处疏密一致。
		// 早先这里是横平竖直的网格配斜着画的文字：斜文字的外接框与正交网格对不上，
		// 列与列之间会空出一条从头通到底的白带、最右一列还可能整列够不到右边 ——
		// "倾斜角度下水印显示不全、右上角没有水印"就是这么来的。
		// 间距按文字尺寸的比例给（档位在工具条上切），小字自动密、大字自动疏
		auto k = sub->getWatermarkGapRatio();
		auto stepU = std::max(textW * (1.f + k), 1.f);   // 沿文字方向
		auto stepV = std::max(textH * (1.f + k), 1.f);   // 垂直文字方向
		auto rad{ rotation * 3.14159265358979323846f / 180.f };
		auto cosR{ cosf(rad) }, sinR{ sinf(rad) };
		// 把图的四个角换算到 (u,v) 取范围：四个角都在里面，格点再各向外扩一格，
		// 四只角就一定被文字压着 —— 不会有哪个角落空着
		float uMin{ 0.f }, uMax{ 0.f }, vMin{ 0.f }, vMax{ 0.f };
		bool first{ true };
		for (auto& p : { D2D1::Point2F(0.f, 0.f), D2D1::Point2F((float)sz.width, 0.f),
			D2D1::Point2F(0.f, (float)sz.height), D2D1::Point2F((float)sz.width, (float)sz.height) })
		{
			auto u{ p.x * cosR + p.y * sinR };
			auto v{ -p.x * sinR + p.y * cosR };
			if (first) { uMin = uMax = u; vMin = vMax = v; first = false; }
			else {
				uMin = std::min(uMin, u); uMax = std::max(uMax, u);
				vMin = std::min(vMin, v); vMax = std::max(vMax, v);
			}
		}
		for (float v = vMin - stepV; v <= vMax + stepV; v += stepV)
		{
			for (float u = uMin - stepU; u <= uMax + stepU; u += stepU)
			{
				// 回到画布坐标：p = u·(cos, sin) + v·(−sin, cos)
				drawOne(ctx, u * cosR - v * sinR, u * sinR + v * cosR, rotation, prev);
			}
		}
	}
	else {
		// 四角 / 上下中 / 居中：只摆一块，而且一律水平摆 —— 角度那一项只对平铺有意义，
		// 工具条上的旋转按钮在非平铺时是置灰的，这里也不去读它
		auto p = anchorPos(pos, (float)sz.width, (float)sz.height);
		drawOne(ctx, p.x, p.y, 0.f, prev);
	}
	ctx->SetTransform(prev);
}

void ShapeWatermark::paintDragger(ID2D1DeviceContext* ctx)
{
	// 与其他元素的夹点不同：水印的"选中框"就是整张底图（平铺时尤其如此），
	// 画一圈边框表示它整体可选中，不做八向夹点 —— 拖动水印没有意义
	auto sz = win->getImgSize();
	if (sz.width == 0 || sz.height == 0) return;
	// 外层变换保持不变：屏幕上跟着缩放走，导出时就是单位阵
	ctx->DrawRectangle(D2D1::RectF(0.f, 0.f, (float)sz.width, (float)sz.height), brushDragger.Get(), 1.f);
}
