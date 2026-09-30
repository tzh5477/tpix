#include "pch.h"
#include "Win/WinPin.h"
#include "Tool/ToolSub.h"
#include "Tool/ToolMain.h"
#include "History.h"
#include "ShapeWatermark.h"

ShapeWatermark::ShapeWatermark(WinPin* win) : ShapeBase(win)
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
	if (!win->toolSub) return false;
	auto text = win->toolSub->watermarkText;
	if (text.empty()) return false;
	// 字号在工具条上是逻辑像素语义，shape 存的是底图像素，换算要乘 dpi
	auto fontSize = win->toolSub->getSliderVal() * win->dpi;
	layout = Ling::D2D::makeTextLayout(text, fontSize);
	if (!layout) return false;
	DWRITE_TEXT_METRICS m{};
	if (FAILED(layout->GetMetrics(&m))) return false;
	textW = m.width;
	textH = m.height;
	// 透明度四档与颜色都在工具条上。alpha 按"用户选的颜色的 alpha × 档位"叠乘，
	// 取白色 + 25% 档就是常见的浅灰水印，不必再单独做一个颜色通道
	auto value = win->toolSub->getSelectedColorValue();
	auto alpha = (float)(value & 0xFF) / 255.f * win->toolSub->getWatermarkOpacity();
	D2D1_COLOR_F c{
		((value >> 16) & 0xFF) / 255.f,
		((value >> 8) & 0xFF) / 255.f,
		(value & 0xFF) / 255.f,
		alpha
	};
	if (FAILED(Ling::D2D::get()->deviceContext->CreateSolidColorBrush(c, brush.GetAddressOf()))) return false;
	return true;
}

void ShapeWatermark::drawOne(ID2D1DeviceContext* ctx, float x, float y, float rotation)
{
	// 平移到落点、旋转、再把布局的中心对齐到原点 —— 这样旋转是绕文字中心转的
	auto m = D2D1::Matrix3x2F::Translation(x, y) * D2D1::Matrix3x2F::Rotation(rotation);
	ctx->SetTransform(m);
	ctx->DrawTextLayout({ -textW / 2.f, -textH / 2.f }, layout.Get(), brush.Get());
}

void ShapeWatermark::paint(ID2D1DeviceContext* ctx)
{
	if (!makeLayout()) return;
	// 不走 win->getImgSize()（那是私有方法），直接问底图要尺寸 —— 平铺范围就是整张底图
	if (!win->screenImg) return;
	auto sz = win->screenImg->GetSize();
	if (sz.width == 0 || sz.height == 0) return;
	auto rotation = win->toolSub->getWatermarkRotation();
	// 屏幕绘制时外层是缩放变换、导出时是单位阵，进来是什么出去还是什么，
	// 不能自己设回 Scale —— 导出图会被放大 scale 倍
	auto prev = ctx->GetTransform();
	if (win->toolSub->watermarkTile) {
		// 平铺：沿水平方向铺满，行距给足一倍字高，密度靠字号自己调
		constexpr float gapX{ 60.f }, gapY{ 40.f };
		auto stepX = textW + gapX;
		auto stepY = textH + gapY;
		// 从负一个步长开始画，保证旋转之后边缘也不会露白
		for (float y = -stepY; y < sz.height + stepY; y += stepY)
		{
			for (float x = -stepX; x < sz.width + stepX; x += stepX)
			{
				drawOne(ctx, x, y, rotation);
			}
		}
	}
	else {
		// 居中：以鼠标落点为水印中心
		drawOne(ctx, cx, cy, rotation);
	}
	ctx->SetTransform(prev);
}

void ShapeWatermark::paintDragger(ID2D1DeviceContext* ctx)
{
	// 与其他元素的夹点不同：水印的"选中框"就是整张底图（平铺时尤其如此），
	// 画一圈边框表示它整体可选中，不做八向夹点 —— 拖动水印没有意义
	if (!win->screenImg) return;
	auto sz = win->screenImg->GetSize();
	auto prev = ctx->GetTransform();
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	ctx->DrawRectangle(D2D1::RectF(0.f, 0.f, (float)sz.width, (float)sz.height),
		brushDragger.Get(), 1.f);
	ctx->SetTransform(prev);
}
