#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"

// 矩形族标注的中间基类。
//
// 矩形几何那一整套交互（八向手柄、整体拖动、旋转、Shift 约束、手柄重算）
// 在这里只写一遍，ShapeRect / ShapeEllipse / ShapeMosaicRect / ShapeEraserRect 都从它派生。
//
// 矩形与椭圆共用同一套几何（轴对齐的 rect + 一个旋转角），区别只在画法与命中，
// 所以"矩形↔圆"互转翻的就是 kind 这一个值，不用把对象换掉（换了会连带
// history / selected 一起动，正在编辑的那一笔也就没了）。
// 这两个连 paint 都在基类里，派生类只设 kind 与自己那份"填不填充"；
// 马赛克与擦除仍然自己接管 paint —— 它们拖拽过程中用的是半透明品红占位色，
// 等 mouseUp 才换成按画面算出来的马赛克 / 擦除画刷。
class ShapeRectBase : public ShapeBase
{
public:
	// 这一笔画出来是矩形还是椭圆。顺序即"互转"按钮的顺序，不落盘，随便调
	enum class Kind { Rect = 0, Ellipse };
	ShapeRectBase(Canvas* win);
	~ShapeRectBase();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	// 选中元素右上角的动作图标要用：矩形族的外接矩形（旋转过的那种）就是它
	bool getShapeBounds(D2D1_RECT_F& out) const override;
	// 工具条上的颜色 / 线宽 / 填充改了，重新取一遍
	void applyStyle() override;
	// 矩形/圆之间互转的那一枚图标。马赛克与擦除同样从这个基类派生，
	// 它们没有"换个形状"这回事，所以由 allowShapeToggle 分开
	int actionCount() const override { return allowShapeToggle ? 1 : 0; }
	void paintActionIcon(ID2D1DeviceContext* ctx, const int i, const D2D1_POINT_2F& c, const float rad) override;
	void onAction(const int i) override;
protected:
	// hoverDraggerIndex 的取值。0~7 是八向手柄（与 makeDraggers 的顺序一一对应），
	// 8 是"整体拖动"，9 是旋转
	enum Hit { HitBody = 8, HitRotate = 9 };
	// 命中八向手柄（传进来的点已经是"逆着旋转转回来"的局部坐标）
	void hitDraggers(const float x, const float y);
	// 命中"整体拖动"（索引 8）。矩形判一圈边框，椭圆判环带
	void hitBody(const float x, const float y);
	// 按当前 rect 重算所有手柄的位置，抬手时与整体移动后都要走一遍
	void makeDraggers();
	// rect 被 mouseDrag 改过之后回调，把由 rect 派生出来的几何重算一遍。
	// 椭圆要用 cx/cy/rx/ry 画与判命中，矩形也有（手柄的直径要按短边算），
	// 所以统一在这里算，派生类需要别的衍生量时覆写它
	virtual void syncFromRect();
	// 首次落笔（shape 还没成形）用哪个手柄当锚点
	virtual int firstDraggerIndex() const;
	// rect 的中心。旋转、手柄都以它为准
	D2D1_POINT_2F rectCenter() const;
	// 第 i 号八向手柄在局部坐标里的中心点
	D2D1_POINT_2F handleLocalPoint(const int i) const;
	// 旋转手柄的方框：按轴对齐的 rect 算完再绕中心转到该在的地方（同 ShapeText）
	void updateRotateHandle();
	// 局部坐标 -> 屏幕坐标（绕 rect 中心转 angle 度）。angle 为 0 时原样返回，
	// 免得白白跑一遍三角函数，也让没转过的那条路径与从前逐位一致
	D2D1_POINT_2F toWorld(const D2D1_POINT_2F& p) const;
	// 把旋转叠进画布当前的变换（屏幕上是缩放、导出时是单位阵），返回叠之前的变换，
	// 画完要 SetTransform 回去。派生类自己接管 paint 时（马赛克、擦除）同样得走它 ——
	// 否则转过的角度只体现在手柄上，图形本身纹丝不动
	D2D1_MATRIX_3X2_F setRotateTransform(ID2D1DeviceContext* ctx) const;
	// 线宽 / 填充记在 config.json 的哪一组。只有矩形与圆这两个"取工具条当前样式"的
	// 会用到：马赛克与擦除的画刷是按画面自己算出来的，跟着工具条走就被涂掉了
	const wchar_t* styleGroup() const;
protected:
	// 底图坐标系里的轴对齐矩形。left/right/top/bottom 由 mouseDrag 按按下点算出
	D2D1_RECT_F rect{ 0,0,0,0 };
	// 手柄方框，下标即 hoverDraggerIndex。没建出来的那几枚是零尺寸框（命中判定恒不成立）
	std::vector<D2D1_RECT_F> draggers;
	Kind kind{ Kind::Rect };
	// 画出来的形状整体绕 rect 中心转过的角度（度）。rect 本身始终轴对齐，旋转只在画的时候
	// 与命中判定时施加，所以导出走同一条 paint 就能得到转过的图，不必额外处理
	float angle{ 0.f };
	// 椭圆几何，由 rect 派生（syncFromRect 里重算）。椭圆一族与矩形共用 rect 这套存法，
	// 画与命中时用它换算成圆心 + 两个半径
	float cx{ 0.f }, cy{ 0.f }, rx{ 0.f }, ry{ 0.f };
	// 拖 0~7 号手柄时固定不动的那个对角点（屏幕坐标）。它每次 mouseDown 重新记，
	// 拖的过程中一直不变 —— 见 mouseDrag 里的说明
	D2D1_POINT_2F anchorWorld{ 0.f, 0.f };
	// 按下时 rect 的宽高。拖上下 / 左右手柄时，另一维要保持这个尺寸
	float pressW{ 0.f }, pressH{ 0.f };
	// 矩形与椭圆共用这一套，但只有它们才允许"互转"。马赛克 / 擦除派生出去时保持 false
	bool allowShapeToggle{ false };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	float strokeWidth{ 1.f }, pressX{ 0.f }, pressY{ 0.f };
	bool isFill{ false };
};
