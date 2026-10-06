#pragma once
#include <include/Ling.h>
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
	// 一端的形状。实心箭头是短宽的实心三角，细箭头是长而窄的开口 V，圆点是个比线粗一圈的实心圆
	enum class EndMark { None = 0, Arrow, Thin, Dot };
	ShapeLine(Canvas* win);
	~ShapeLine();
	void paint(ID2D1DeviceContext* ctx) override;
	void applyStyle() override;
	// 直角折线的几何由这几个覆写接管，理由见 .cpp
	void mouseDown(const float x, const float y) override;
	void mouseDrag(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	// 光标停在这条线上滚滚轮 = 调线宽
	void mouseWheel(const float x, const float y, const short delta) override;
private:
	bool isOrtho() const;
	// 把这次拖拽的鼠标轨迹压成一条横平竖直的折线
	void snapTrail();
	// 已经画好的这一笔改成直角折线：拿它现有的顶点重吸附一遍（见 .cpp）
	void snapExisting();
	// 当前端点档位下两端各画什么
	struct EndPair { EndMark start, end; };
	EndPair ends() const;
	void paintEnds(ID2D1DeviceContext* ctx);
	void paintEnd(ID2D1DeviceContext* ctx, const D2D1_POINT_2F& tip, const D2D1_POINT_2F& dir, const EndMark mark);
	// 这一端该朝哪个方向：从端点沿折线往回让够一段再连线（见 .cpp）
	D2D1_POINT_2F endDir(bool atEnd) const;
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
