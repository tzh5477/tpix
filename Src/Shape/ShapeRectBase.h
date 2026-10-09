#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"

// 矩形族标注的中间基类。
//
// 矩形几何那一整套交互（八向手柄、整体拖动、旋转、圆角、扇区、Shift 约束、手柄重算）
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
	bool mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	// 选中元素右上角的动作图标要用：矩形族的外接矩形（旋转过的那种）就是它
	bool getShapeBounds(D2D1_RECT_F& out) const override;
	// 工具条上的颜色 / 线宽 / 填充改了，重新取一遍
	void applyStyle() override;
	// 批量旋转（见 ShapeBase::rotateBy）：位置绕 center 转 + 自身角度加 deg
	void rotateBy(const float deg, const D2D1_POINT_2F& center) override;
	// 工具条上「几何图形」选了哪一类（矩形 / 圆形），把这一笔翻成那一类。
	// 元素左下角原来有一枚互转图标干这件事，撤掉之后换类别只在工具条上做得到
	// （见 ToolSub::makeGeomKindBtns），所以选中态跟着工具条走 —— 与箭头样式同一条路。
	// 马赛克与擦除没有"换个形状"这回事，由 useToolStyle 分开
	void applyToolStyle() override;
protected:
	// 复制（见 ShapeBase::clone）。这一族只多两件事：rect 挪开、自己那支画刷重建一份，
	// 具体是"哪个类"由派生类各写一行 cloneSelf
	void fixupCopy() override;
	void translate(const float dx, const float dy) override;
	// hoverDraggerIndex 的取值。0~7 是八向手柄（与 makeDraggers 的顺序一一对应），
	// 8 是"整体拖动"，9 起是本族新增的：
	// 9 旋转、10~13 圆角（左上/右上/右下/左下）、14 扇区内径、15/16 扇区缺角的起始边/终止边。
	// ⚠️ 圆角那四枚现在**只有 10（左上）真的建得出来**（作者：四枚太密，收成一枚）——
	// 11~13 留着占位，是给"绘制 / 命中"那两处 for 循环用的，别顺手把它们当成还能拖
	enum Hit {
		HitBody = 8, HitRotate = 9, HitRadiusTL = 10, HitRadiusTR, HitRadiusBR, HitRadiusBL,
		HitInner = 14, HitNotchStart, HitNotchEnd
	};
	// 命中八向手柄与那几个内部手柄（传进来的点已经是"逆着旋转转回来"的局部坐标）
	void hitDraggers(const float x, const float y);
	// 命中"角手柄外侧那一圈"（旋转）。传进来的同样是局部坐标。
	// 旋转原来独占右下角一个坑，与八向手柄挤在同一圈窄带里；改成角外的环带之后不占位置。
	// 另外它先认那枚**看得见的**旋转图标（见 rotateHint）—— 所见即所点
	void hitRotateBand(const float x, const float y);
	// 按当前外接框算出那枚看得见的旋转图标的位置（见 rotateHint 的说明）。
	// 摆放、命中、绘制三处都走它，避免各算一份对不齐
	void makeRotateHint();
	// 命中"整体拖动"（索引 8）。矩形判一圈边框，椭圆（含扇形 / 环形）判环带
	void hitBody(const float x, const float y);
	// 按当前 rect 重算所有手柄的位置，抬手时与整体移动后都要走一遍
	void makeDraggers();
	// rect 被 mouseDrag 改过之后回调，把由 rect 派生出来的几何重算一遍。
	// 椭圆要用 cx/cy/rx/ry 画与判命中，矩形也有（圆角手柄的直径要按短边算），
	// 所以统一在这里算，派生类需要别的衍生量时覆写它
	virtual void syncFromRect();
	// 首次落笔（shape 还没成形）用哪个手柄当锚点
	virtual int firstDraggerIndex() const;
	// 这一族有没有"内部手柄"（矩形的圆角那四枚、椭圆的扇区那三枚）。
	// 图片（ShapeImage）从本类借几何，但那几枚一个都用不上 —— 它没有圆角也没有扇区，
	// 手柄画出来、拖起来却什么都不改，只会让人以为图片能调圆角。覆写成 false 关掉。
	// 注意"整体拖动"（HitBody）与旋转（HitRotate）不在这条线上，它们照旧生效
	virtual bool hasInnerHandles() const { return true; }
	// rect 的中心。旋转、手柄、扇区都以它为准
	D2D1_POINT_2F rectCenter() const;
	// 第 i 号八向手柄在局部坐标里的中心点
	D2D1_POINT_2F handleLocalPoint(const int i) const;
	// 扇形 / 环形那条闭合路径（局部坐标）。没有缺角时调用方直接画椭圆，不必走它
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> makePieGeometry() const;
	// 画一枚"内部手柄"（圆角 / 扇区那几枚）。它们是空心的圆点，
	// withCenter 为真时中间再点一个实心点 —— 那是"转方向"那一枚
	void paintDot(ID2D1DeviceContext* ctx, const D2D1_RECT_F& box, const bool withCenter) const;
	// 局部坐标 -> 屏幕坐标（绕 rect 中心转 angle 度）。angle 为 0 时原样返回，
	// 免得白白跑一遍三角函数，也让没转过的那条路径与从前逐位一致
	D2D1_POINT_2F toWorld(const D2D1_POINT_2F& p) const;
	// 把旋转叠进画布当前的变换（屏幕上是缩放、导出时是单位阵），返回叠之前的变换，
	// 画完要 SetTransform 回去。派生类自己接管 paint 时（马赛克、擦除）同样得走它 ——
	// 否则转过的角度只体现在手柄上，图形本身纹丝不动
	D2D1_MATRIX_3X2_F setRotateTransform(ID2D1DeviceContext* ctx) const;
	// 椭圆参数方程上的点：deg 按屏幕习惯（0 度朝右、正角度顺时针），scale 是半径的倍数
	D2D1_POINT_2F ellipsePoint(const float deg, const float scale) const;
	// 圆角手柄"初始位置"离角点的距离。它随图形尺寸走（小图形不该被手柄压满），
	// 但最小也得让开角上那个八向手柄
	float radiusHome() const;
	// 圆角半径的上限：再大就把对角方向也吃掉了
	float radiusMax() const;
	// 扇区模型：缺角从 notchStart 顺时针扫 notchSweep 度被挖掉，
	// 剩下的那段（从 notchStart+notchSweep 一直扫回 notchStart）才是要画的部分
	bool isPie() const { return kind == Kind::Ellipse && notchSweep > 0.5f; }
	// deg 是否落在被挖掉的那段缺角里
	bool inNotch(const float deg) const;
	// 把角度归一化到 [0, 360)
	static float norm360(const float deg);
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
	// 圆角半径（底图像素）。只有矩形用，椭圆那边恒为 0
	float radius{ 0.f };
	// 扇区：缺角起点（度）、缺角跨度（度，0 = 完整圆）、内半径比例（0 = 实心扇形，>0 = 环形）。
	// 起点取正上方（-90 度）：完整圆上那一枚小点在正上方，往旁边拖就是"从 12 点切一刀"
	float notchStart{ -90.f }, notchSweep{ 0.f }, innerRatio{ 0.f };
	// 拖 0~7 号手柄时固定不动的那个对角点（屏幕坐标）。它每次 mouseDown 重新记，
	// 拖的过程中一直不变 —— 见 mouseDrag 里的说明
	D2D1_POINT_2F anchorWorld{ 0.f, 0.f };
	// 按下时 rect 的宽高。拖上下 / 左右手柄时，另一维要保持这个尺寸
	float pressW{ 0.f }, pressH{ 0.f };
	// 增量旋转的起点：按下那一刻自身的角度，与鼠标相对中心的方向（度）。
	// 拖动时按"转过了多少"往上叠加（见 mouseDrag 的 HitRotate 分支）——
	// 不用"鼠标方向 - 手柄静止方向"那种绝对式，手柄静止方向得按当前 angle 现算，
	// 而拖动过程中 angle 正在变，两者互相依赖会漂
	float rotateStartAngle{ 0.f }, rotateStartDir{ 0.f };
	// 那枚**看得见的**旋转图标（底图坐标）。选中（或悬停）时常驻在外接框右下角外侧，
	// 贴到画布右下边缘放不下就翻到框内侧 —— 与 ShapeText 那枚独立手柄同一条规矩。
	//
	// 为什么要它：环带那套命中本身没问题，但图标原来只在"光标已经落进环带"时才画，
	// 而那句判据写错了（rotateCorner 只可能被 hitRotateBand 设成 0/2/4/6，
	// 条件却写的 `>= 100`）⇒ 一枚都画不出来。作者报的"选中之后看不到可旋转的标识、
	// 只能盲操"就是这个。现在图标常驻，命中它本人也算抓住旋转
	D2D1_RECT_F rotateHint{ 0.f, 0.f, 0.f, 0.f };
	// 这一族取不取工具条上"当前那一套样式"（颜色 / 线宽 / 填充 / 类别）。
	// 只有矩形与圆这两个"照工具条画"的会用到：马赛克与擦除的画刷是按画面自己算出来的，
	// 跟着工具条走就被涂掉了，它们也不参与"换个形状"
	bool useToolStyle{ false };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	float strokeWidth{ 1.f }, pressX{ 0.f }, pressY{ 0.f };
	bool isFill{ false };
};
