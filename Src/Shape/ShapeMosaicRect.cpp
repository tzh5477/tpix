#include "pch.h"
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeMosaicRect.h"

ShapeMosaicRect::ShapeMosaicRect(Canvas* win) :ShapeRectBase(win), mosaicPaint{ win, this }
{
	auto toolSub = win->getToolSub();
	auto d2d = Ling::D2D::get();
	// 马赛克没有颜色可选，这个半透明品红只在拖拽过程中当占位提示 ——
	// 松开鼠标后 buildMosaic / buildErase 会算出真正的画刷把它替换掉
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xF00FF0, 0.38f), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isErase = toolSub->mosaicMode == 2;
	// 这里置 isFill 不是"填充矩形工具"的意思：这块是整块填充、没有可摸的边框，
	// hitBody 因此按固定 2*dpi 宽判命中带，而不是按线宽 —— 与滑块笔刷尺寸无关（见基类）
	isFill = true;
}

ShapeMosaicRect::~ShapeMosaicRect()
{

}

std::unique_ptr<ShapeBase> ShapeMosaicRect::clone(const float dx, const float dy, Canvas* target) const
{
	return cloneSelf(*this, dx, dy, target);
}

void ShapeMosaicRect::fixupCopy()
{
	ShapeRectBase::fixupCopy();
	// mosaicPaint 里存着"我是哪一个 shape"（它按"画到自己为止"回读画面来算），拷过来
	// 还指着原件 —— 不换掉的话，新的一份会把原件自己也算进画面里
	mosaicPaint = ShapeMosaicPaint(win, this);
}

void ShapeMosaicRect::translate(const float dx, const float dy)
{
	ShapeRectBase::translate(dx, dy);
	// 挪了地方，原来那一块画面的马赛克就不对了 —— 照 mouseUp 那条路重算一遍
	resetMosaic();
	if (isErase) {
		buildErase();
		return;
	}
	buildMosaic();
}

// 马赛克块大小是从 strokeWidth 折算的，改线宽要按新块大小重新生成马赛克位图。
// 只在已经生成过时重建 —— 还没 mouseUp 的那一笔交回 paint 自己处理
void ShapeMosaicRect::applyStyle()
{
	strokeWidth = win->getToolSub()->getSliderVal();
	if (mosaicBrush) buildMosaic();
}

void ShapeMosaicRect::paint(ID2D1DeviceContext* ctx)
{
	// 旋转由基类统一叠（马赛克也能转），画完要把变换还回去 —— 后面还有别的东西要画
	auto prev{ setRotateTransform(ctx) };
	if (eraseBrush) {
		ctx->FillRectangle(rect, eraseBrush.Get());
	}
	else if (mosaicBrush) {
		ctx->FillRectangle(rect, mosaicBrush.Get());
	}
	else {
		ctx->FillRectangle(rect, brush.Get());
	}
	ctx->SetTransform(prev);
}

// 几何一变，之前算好的马赛克就不对了，先扔掉退回占位色
void ShapeMosaicRect::mouseDrag(const float x, const float y)
{
	resetMosaic();
	ShapeRectBase::mouseDrag(x, y);
}

void ShapeMosaicRect::mouseUp(const float x, const float y)
{
	ShapeRectBase::mouseUp(x, y);
	// 几何定下来了才算马赛克 —— 这一步要把 GPU 像素读回内存，拖拽过程中每帧做太贵
	if (isErase) {
		buildErase();
		return;
	}
	buildMosaic();
}

void ShapeMosaicRect::resetMosaic()
{
	mosaicBrush.Reset();
	// 智能擦除的取样色不跟着每一次拖动重算：取样要走一遍 GPU 读回 + 直方图统计，
	// 而拖动是按帧来的。留着上一次的色，等 mouseUp 再取一次，比退回占位色好看得多
	if (!isErase) eraseBrush.Reset();
}

void ShapeMosaicRect::buildMosaic()
{
	// 块越大越糊。滑块调的是笔画粗细，顺带让粗笔画对应大色块，下限 6px 保证肉眼可见
	int blockSize = std::max(6, (int)std::round(strokeWidth / 3.0f));
	mosaicBrush = mosaicPaint.makeMosaicBrush(rect, blockSize);
}

void ShapeMosaicRect::buildErase()
{
	D2D1_COLOR_F color{};
	if (!mosaicPaint.sampleBgColor(rect, 4, color)) return;
	Ling::D2D::get()->deviceContext->CreateSolidColorBrush(color, eraseBrush.GetAddressOf());
	win->refresh();
}
