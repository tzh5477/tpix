#pragma once
#include <include/Ling.h>
#include "ShapeRectBase.h"
#include "ShapeMosaicPaint.h"
// 矩形马赛克，以及借它几何的「智能擦除」。
// 马赛克面板只剩两档（见 ToolSub::mosaicMode）：矩形走本类、涂抹走 ShapeMosaicLine。
// 橡皮擦面板的「智能擦除」档（ToolSub::eraserMode == 2）也用本类 —— 两者几何完全一样
// （拖一个矩形），区别只在"几何定下来之后填什么"：马赛克填马赛克，智能擦除填
// 从选区周围取来的那个背景色（见构造函数的 smartErase 与 isErase）。
//
// 智能打码（只打数字）也在这个类里，但它**不是一个模式、也不来自鼠标**：
// 工具条上单有一枚按钮，按一下就扫全图把所有数字打掉（见 buildSmartArea）。
// 它的几何由调用方直接给（整张底图），所以不走 press / drag / up 那一条
//
// 八向手柄、整体拖动、Shift 约束整套来自 ShapeRectBase，这里只负责
// "几何定下来之后把那块画面打成马赛克 / 取背景色 / 按识别结果挑出要打的段落"
class ShapeMosaicRect : public ShapeRectBase
{
public:
	// smartErase：造出来就是"智能擦除"那一笔（橡皮擦工具的第三档）。
	// 不由类自己去读 ToolSub 的档位猜 —— 马赛克与橡皮擦都可能造本类，
	// 光看 eraserMode 会把"矩形马赛克"也当成智能擦除
	ShapeMosaicRect(Canvas* win, bool smartErase = false);
	~ShapeMosaicRect();
	void paint(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void applyStyle() override;
	// 智能打码在选区里一段数字都没认出来时等于什么都没画（它是不可见的），
	// 交给 WinPin::onUp 的空笔那条路直接丢掉
	bool isEmptyStroke() const override { return isSmart && masks.empty(); }
	// 一键智能打码（工具条上那枚星形按钮走这条）：**不给鼠标那条路走** ——
	// 它中途要 press / anchor 那一套来推进几何，而这里几何是现成的（整张底图）。
	// 直接把 rect 设成 area、按识别出来的数字算出要铺的框，语义与"拖着框选一遍"
	// 完全一样，只是框是调用方给的、不来自鼠标
	void buildSmartArea(const D2D1_RECT_F& area);
	// 算完之后落下几个数字框。0 表示这张图里（或这个区域里）一个数字都没认出来，
	// 调用方据此判断"这一笔不留"，别在图上多一个看不见的空元素
	bool hasMasks() const { return !masks.empty(); }
	// 复制（见 ShapeBase::clone）
	bool copyable() const override { return true; }
	std::unique_ptr<ShapeBase> clone(const float dx, const float dy, Canvas* target = nullptr) const override;
	std::unique_ptr<ShapeBase> snapshot() const override;
protected:
	void fixupCopy() override;
	void translate(const float dx, const float dy) override;
private:
	// 打好的马赛克画刷。与 eraseBrush 互斥：一个 shape 只会走到其中一条路上
	Microsoft::WRL::ComPtr<ID2D1BitmapBrush> mosaicBrush;
	// 智能擦除：打听选区周围那圈出现最多的颜色当背景色，用它铺满选区
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> eraseBrush;
	void resetMosaic();
	void buildMosaic();
	int mosaicBlockSize() const;
	void buildErase();
	// 智能打码：挑出要打码的数字，存进 masks 再建位图刷。
	// 有字符级框就逐字判、只打数字字符；没有（系统 OCR / 大模型）才回落到整段判
	void buildSmartMasks();
	// 同一行里挨着的几段并成一段，并把这一行的框统一上下边界
	void mergeMasksByRow();
	// 框的对边对到马赛克格子上（只往外扩），半格不会漏出笔画的淡影。
	// snapX=false 只对纵向 —— 字符级那条路算框时已经对过横向、并且被邻居的墨夹过，
	// 这里再扩一次会把夹好的边又扩回去（见 .cpp 里的说明）
	void snapMasksToBlocks(const int blockSize, const bool snapX);
private:
	bool isErase{ false };
	bool isSmart{ false };
	// 智能打码要铺的框（底图坐标）。它是这一笔的**内容**：拖位置 / 改选区时跟着动，
	// 撤销栈快照与复制都照隐式拷贝走，不必额外处理
	std::vector<D2D1_RECT_F> masks;
	ShapeMosaicPaint mosaicPaint;
};
