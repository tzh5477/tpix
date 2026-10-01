#pragma once
#include <include/Ling.h>

// 光标落在哪一块。选区内部是 Inside，外面被选区四边的延长线切成 8 块，各对应一个方位
enum class MaskHit { None, Inside, Left, Top, Right, Bottom, TopLeft, TopRight, BottomRight, BottomLeft };

// 框选遮罩。四块半透明遮罩 + 蓝色选区边框 + 左上角尺寸标签。
// 自己不持有绘制目标，paint 时由宿主窗口把当前的 context 传进来。
// 所有坐标都是宿主窗口的客户区坐标。
class CutMask
{
public:
	CutMask(Ling::WinBase* win);
	// 鼠标悬停时吸附到光标下的窗口矩形，选区真的变了才返回 true
	bool highlight(POINT pos);
	void startMakeRect(POINT pos);
	void makeRect(POINT pos);
	// 选区四边的延长线把窗口切成九块：中间是 Inside，外面八块各是一个方位。没框出选区时才是 None
	MaskHit hitTest(POINT pos) const;
	// 开始调整：记下方向和起始矩形。按的是边或角时，这一下就把那条边吸到光标处
	void startAdjust(POINT pos);
	// 调整中：按 startAdjust 记下的方向改选区
	void adjust(POINT pos);
	// 宽高都大于 0 才算真的框出了东西
	bool hasRect() const;
	void paint(ID2D1DeviceContext* ctx);

	// —— 手绘（自由多边形）选区 ——
	// 拖动过程中逐点采样，松手闭合成多边形。maskRect 始终是它的外接矩形，
	// 于是工具条定位、长截图、录屏这些只看 maskRect 的逻辑一行都不用改；
	// 多边形以外的像素要透明，那是 getCutImg() 才需要操心的事
	bool isPoly() const { return poly.size() >= 3; }
	void startPoly(POINT pos);
	void addPolyPoint(POINT pos);
	// 松手：收掉首尾重合的点。点数不足就整体作废，等于没框出东西
	void endPoly();
	void clearPoly();
	// 多边形转 D2D 几何，供 getCutImg 做透明遮罩
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> makePolyGeom(int offsetX, int offsetY) const;
public:
	D2D1_RECT_F maskRect{};
	float strokeWidth{ 2.f };
	// 选区定死之后（录屏 / 滚动截图）标签就没用了，而它可能压在选区内部被录进去
	bool hideLabel{ false };
private:
	void initWinRect();
	// 尺寸标签只在 maskRect 变化时重建，不必每帧现建
	void makeLayout();
	// 手绘路径变化时重建：外接矩形、尺寸标签、以及"挖了洞的那块蒙层几何"
	void syncPoly();
	// 偶数-奇数填充（Even-Odd）判定点是否落在多边形内部
	bool pointInPoly(POINT pos) const;
	void movePoly(const float dx, const float dy);
private:
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBorder;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
	D2D1_RECT_F layoutRect{};
	std::vector<D2D1_RECT_F> winRect;
	std::vector<D2D1_POINT_2F> poly;
	// 整个窗口矩形减去多边形那一块（偶数-奇数填充天然成环）。paint 的蒙层直接用它
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> ringGeom;
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> polyGeom;
	// 采样间距：光标移动得再快，也别把路径点录得太密（构造里乘过 dpi，物理像素）
	float polyStep{ 4.f };
	POINT pressPos{};
	Ling::WinBase* win{ nullptr };
	float paddingTop{ 2.f }, paddingMargin{3.f};
	MaskHit adjustHit{ MaskHit::None };
	// 调整全程以按下那一刻的矩形为基准算，不做累加，免得漂移
	D2D1_RECT_F adjustStartRect{};
	POINT adjustPressPos{};
	// 选区最小尺寸，别让它塌成 0 宽 0 高
	static constexpr float minSize{ 4.f };
};
