#pragma once
#include <include/Ling.h>
#include <wrl.h>
#include <string>
#include <unordered_map>
#include <utility>

// 屏幕上的三种辅助层。都是铺满虚拟桌面的顶层窗口，Esc 或再点一次托盘项关闭。
// ruler = 上边 / 左边各一条像素标尺；crosshair = 跟随光标的十字准线；
// focus = 只留一个亮区，其余压暗（拖动亮区移动，滚轮缩放）
enum class OverlayMode { Ruler, Crosshair, Focus };

class WinOverlay : public Ling::WinBase
{
public:
	WinOverlay(OverlayMode mode);
	~WinOverlay();
	// 已经是同一种辅助层就当"关掉"，等于开关
	static void toggle(OverlayMode mode);
	static void dispose();
	static bool isOpen(OverlayMode mode);
private:
	void onCreated() override;
	void layout() override;
	void paintRuler(ID2D1DeviceContext* ctx);
	void paintCrosshair(ID2D1DeviceContext* ctx);
	void paintFocus(ID2D1DeviceContext* ctx);
	// 带半透明底的一段文字，返回它占的宽高，方便调用方算摆放位置
	D2D1_SIZE_F drawLabel(ID2D1DeviceContext* ctx, const std::wstring& text, float x, float y, float fontSize);
	// 亮区别被拖出屏幕，也别缩成看不见
	void clampHole();
private:
	OverlayMode mode;
	Ling::Canvas* canvas{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushLine;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText;
	// 光标位置，客户区坐标。标尺与十字是鼠标穿透的，收不到鼠标事件，靠定时器读
	POINT cursor{ 0, 0 };
	bool isDragging{ false };
	POINT dragFrom{ 0, 0 };
	// focus 的亮区，物理像素、屏幕坐标
	int holeX{ 0 }, holeY{ 0 }, holeW{ 0 }, holeH{ 0 };
	// 标尺刻度那些文字每帧都要画一遍，重排版一次几毫秒、几十处就是肉眼可见的卡。
	// 按"文字 + 字号"缓存，只有坐标标签这种真会变的才每帧新建
	std::unordered_map<std::wstring, std::pair<float, Microsoft::WRL::ComPtr<IDWriteTextLayout>>> labelCache;
};
