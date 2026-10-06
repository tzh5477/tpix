#pragma once
#include <include/Ling.h>
#include "ShapeRectBase.h"

// 图片标注：把一张位图当成可拖动 / 可缩放的元素贴进截图里。
// 入口是编辑器里的 Ctrl+V —— 系统剪贴板上是图时由 WinPin::pasteFromSystemClipboard
// 建一个它插进当前画布（作者要的"复制了图直接粘进截图"）。
//
// 几何那一整套（八向手柄缩放、旋转手柄、整体拖动、命中判定）全部从 ShapeRectBase 借来，
// 本类只做两件事：接管 paint 画位图，以及把命中扩到"整块"（见 mouseMove）。
class ShapeImage : public ShapeRectBase
{
public:
	ShapeImage(Canvas* win);
	~ShapeImage();
	// 装图。px 是 BGRA、top-down、行紧凑（步长 = w*4），与 Util::readClipboard 交出来的
	// 那套格式一致。内部会预乘一份留着 —— DrawBitmap 只认预乘 alpha 的位图，
	// 而剪贴板 / WIC 给的是 straight alpha，不预乘的话半透明边缘会泛白
	bool setImage(const std::vector<BYTE>& px, const int w, const int h);
	// 摆到哪、占多大（底图像素）。粘贴时由 WinPin 按落点与画布尺寸算好。
	// 定完要重算手柄，否则要等第一次 hover 才认得出手柄在哪
	void placeAt(const float x, const float y, const float w, const float h);
	void paint(ID2D1DeviceContext* ctx) override;
	void mouseMove(const float x, const float y) override;
	// 图片能复制（见 ShapeBase::copyable）：左上角那枚复制按钮据此出现，
	// Ctrl+C / Ctrl+V 也就能把贴进来的图再复制几份
	bool copyable() const override { return true; }
	// 这一族没有圆角也没有扇区，基类那几枚内部手柄点画出来只会让人以为能调（见基类）
	bool hasInnerHandles() const override { return false; }
	std::unique_ptr<ShapeBase> clone(const float dx, const float dy, Canvas* target = nullptr) const override;
protected:
	void fixupCopy() override;
private:
	// 按当前的 pixels / imgW / imgH 建 D2D 位图
	bool makeBitmap();
	// 源像素（已预乘）。留着它是因为位图是设备资源 —— 复制出来的一份必须自己重建
	//（与画刷同一条理由，见 ShapeBase::cloneSelf），没有源像素就重建不出来
	std::vector<BYTE> pixels;
	int imgW{ 0 }, imgH{ 0 };
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
};
