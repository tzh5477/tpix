#include "pch.h"
#include "Canvas.h"
#include "Tool/ToolSub.h"
#include "Tool/ToolMain.h"
#include "History.h"
#include "ShapeWatermark.h"

ShapeWatermark::ShapeWatermark(Canvas* win) : ShapeBase(win)
{
}

ShapeWatermark::~ShapeWatermark()
{
}

void ShapeWatermark::mouseDown(const float x, const float y)
{
	// 居中模式的落点。平铺模式下用不到它，但记着无妨
	cx = x;
	cy = y;
}

void ShapeWatermark::setCursor()
{
	// 与序号一致：水印是"点一下就落"的元素，用系统箭头就够了
	SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

bool ShapeWatermark::makeLayout()
{
	layout.Reset();
	brush.Reset();
	textW = 0.f;
	textH = 0.f;
	if (!win->getToolSub()) return false;
	auto text = win->getToolSub()->watermarkText;
	if (text.empty()) return false;
	// getSliderVal 返回的已经是物理像素（内部乘过 dpi），这里再乘一次会变成 dpi² ——
	// 150% 缩放下 24 号被算成 54，字被放大、平铺步长跟着变大，看着就是"稀得看不见字"
	auto fontSize = win->getToolSub()->getSliderVal();
	layout = Ling::D2D::makeTextLayout(text, fontSize);
	if (!layout) return false;
	DWRITE_TEXT_METRICS m{};
	if (FAILED(layout->GetMetrics(&m))) return false;
	textW = m.width;
	textH = m.height;
	// 透明度四档与颜色都在工具条上。alpha 取"档位"这一个值就够了 ——
	// 色板里每种颜色自己的 alpha 恒是 0xFF，乘不乘没区别
	auto alpha = win->getToolSub()->getWatermarkOpacity();
	// 走 getSelectedColor 与其它标注同一条解码路径。原来这里手写移位把 0xRRGGBBAA 拆错位
	// （每一路都少移 8 位，R 取成 G、B 取成 A），调色板的 alpha 又恒是 0xFF，
	// 于是任何颜色都带满蓝：红色 0xCF1322FF 被解成 (19,34,255) —— 画出来就是蓝的
	auto c = win->getToolSub()->getSelectedColor();
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

void ShapeWatermark::paint(ID2D1DeviceContext* ctx)
{
	if (!makeLayout()) return;
	// 不走 win->getImgSize()，直接问底图要尺寸 —— 平铺范围就是整张底图
	if (!win->screenImg) return;
	auto sz = win->screenImg->GetSize();
	if (sz.width == 0 || sz.height == 0) return;
	auto rotation = win->getToolSub()->getWatermarkRotation();
	// 屏幕绘制时外层是缩放变换、导出时是单位阵，进来是什么出去还是什么，
	// 不能自己设回 Scale —— 导出图会被放大 scale 倍
	D2D1_MATRIX_3X2_F prev{};
	ctx->GetTransform(&prev);
	if (win->getToolSub()->watermarkTile) {
		// 平铺：步长 = 文字尺寸 + 间距，间距按文字尺寸的比例给（档位在工具条上切），
		// 小字自动密、大字自动疏 —— 早先写死 60/40 像素，配 10 号小字就是一片空白里的零星几个字
		auto k = win->getToolSub()->getWatermarkGapRatio();
		auto stepX = textW + textW * k;
		auto stepY = textH + textH * k;
		// 从负一个步长开始画，保证旋转之后边缘也不会露白
		for (float y = -stepY; y < sz.height + stepY; y += stepY)
		{
			for (float x = -stepX; x < sz.width + stepX; x += stepX)
			{
				drawOne(ctx, x, y, rotation, prev);
			}
		}
	}
	else {
		// 居中：以鼠标落点为水印中心
		drawOne(ctx, cx, cy, rotation, prev);
	}
	ctx->SetTransform(prev);
}

void ShapeWatermark::paintDragger(ID2D1DeviceContext* ctx)
{
	// 与其他元素的夹点不同：水印的"选中框"就是整张底图（平铺时尤其如此），
	// 画一圈边框表示它整体可选中，不做八向夹点 —— 拖动水印没有意义
	if (!win->screenImg) return;
	auto sz = win->screenImg->GetSize();
	// 外层变换保持不变：屏幕上跟着缩放走，导出时就是单位阵
	ctx->DrawRectangle(D2D1::RectF(0.f, 0.f, sz.width, sz.height), brushDragger.Get(), 1.f);
}
