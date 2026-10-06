#pragma once
#include <include/Ling.h>
#include <utility>
#include "ShapeLineBase.h"
// 直线：折线族里"取工具条当前颜色描一条线"的那个。
// 拖首端、拖末端、整体平移、线段命中判定都在 ShapeLineBase，这里管的是"这条线长什么样"：
//   1) 线条类型 —— 直角折线（默认）还是普通线条。前者拖拽时按鼠标轨迹吸附成横平竖直，
//      后者维持自由画（每个鼠标位置塞一个顶点）
//   2) 两端的形状 —— 无 / 实心箭头 / 细箭头 / 圆点 的十种组合
//   3) 线条样式 —— 实线 / 虚线 / 波浪线 / 点状线 / 长短虚线 / 删除线
// 三样都从 ToolSub 现取。改完立刻套到图上选中的那一笔上（没选中就只影响之后新画的）；
// 要批量改图上同类的用「全」
class ShapeLine : public ShapeLineBase
{
public:
	// 线条类型。顺序 = ToolSub::lineKind 的落盘值，不能随手调
	enum class Kind { Ortho = 0, Free };
	// 线条样式。同上，顺序 = ToolSub::lineStyle 的落盘值
	enum class Style { Solid = 0, Dash, Wave, Dot, DashDot, DashDotDot };
	// 一端的形状。两种箭头都照 FSCapture 的端点预览定比例（见 .cpp 里的 paintEnd）：
	// 实心那档是平底三角，细的那档是后缘带凹口的燕尾；圆点是个比线粗一圈的实心圆
	enum class EndMark { None = 0, Arrow, Thin, Dot };
	ShapeLine(Canvas* win);
	~ShapeLine();
	void paint(ID2D1DeviceContext* ctx) override;
	void applyStyle() override;
	void applyToolStyle() override;
	// 直角折线的几何由这几个覆写接管，理由见 .cpp
	void mouseDown(const float x, const float y) override;
	void mouseDrag(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	// 光标停在这条线上滚滚轮 = 调线宽
	void mouseWheel(const float x, const float y, const short delta) override;
	// ---- 工具条上那两个下拉里的一格预览 ----
	// 与真正画线的那几条路共用同一份几何与比例（paintMark / dashOf / 波浪那组常数）。
	// 预览要是自己另画一套，等哪天照 FSCapture 再调一次箭头形状，
	// 下拉里看到的就和画到图上的不是一个东西了。
	// rect 是这一格的范围（物理像素），strokeW 是预览线的粗细（物理像素）；
	// 线一律从最左画到最右 —— 每个枚举值一样长，不会出现"直线很长、带箭头的却很短"
	static void paintEndSample(ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect, int endIndex,
		float strokeW, ID2D1Brush* brush);
	static void paintStyleSample(ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect, int styleIndex,
		float strokeW, ID2D1Brush* brush);
private:
	bool isOrtho() const;
	// 把这次拖拽的鼠标轨迹压成一条横平竖直的折线
	void snapTrail();
	// 已经画好的这一笔改成直角折线：拿它现有的顶点重吸附一遍（见 .cpp）
	void snapExisting();
	// 当前端点档位下两端各画什么。用 pair 而不是自己那个两字段的小结构：
	// 档位表要与下拉预览共用一份，而那张表在 .cpp 的匿名 namespace 里
	using EndPair = std::pair<EndMark, EndMark>;
	EndPair ends() const;
	void paintEnds(ID2D1DeviceContext* ctx);
	void paintEnd(ID2D1DeviceContext* ctx, const D2D1_POINT_2F& tip, const D2D1_POINT_2F& dir, const EndMark mark);
	// 这一端该朝哪个方向：从端点沿折线往回让够一段再连线（见 .cpp）
	D2D1_POINT_2F endDir(bool atEnd) const;
	// 画出来的那条线用哪串点：两端有标记的那一头往回收一截（理由见 .cpp 里的 kMarkInset）。
	// linePoints 本身不能动 —— 端点朝向、夹点位置、标记落点都按它算
	std::vector<D2D1_POINT_2F> shaftPoints() const;
	void makePath() override;
	// 命中判定不挖笔画正中那一块（覆写理由见 ShapeLineBase::hitInnerLimit）：
	// 线宽上限是 60 逻辑像素，挖掉半个夹点宽之后只有紧贴边缘的一圈能点中，
	// 粗线的杆"点哪儿都没反应"（作者报的"点箭头线条无法选中"）
	float hitInnerLimit(const float outer) const override;
	// 外接框多让出去一截：两端那几档标记（实心箭头半宽 3.5 倍线宽、圆点半径 1.85 倍）
	// 横着支在端点两侧，只让半个线宽的话，框选时只碰到箭翼尖上那一点会漏掉整支箭
	float boundsPad() const override;
	// 波浪线那条正弦路径。D2D 的虚线样式里没有波浪，只能把折线重采样
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> makeWaveGeometry() const;
	// 按当前线条样式建描边样式（实线 / 虚线族）；波浪不走它
	void makeStrokeStyle();
private:
	// 一次拖拽期间的原始鼠标轨迹。只在"新建这一笔"时有值，抬手就清掉
	std::vector<D2D1_POINT_2F> trail;
	bool creating{ false };
	Kind kind{ Kind::Ortho };
	Style lineStyle{ Style::Solid };
	int endIndex{ 0 };
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> strokeStyle;
};
