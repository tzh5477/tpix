#pragma once
#include <include/Ling.h>
#include <shellapi.h>

// 悬浮球：常驻屏幕边缘的一枚小圆球，点一下弹快捷菜单，图片文件拖到它上面直接贴图。
// 拖放走 WM_DROPFILES（见 .cpp 里的说明）：资源管理器拖出来的本来就是文件路径，
// 不必为了 OLE 的 IDropTarget 去动 Ling —— Ling 也不转发 WM_DROPFILES，只能自己挂一层窗口过程。
class WinBall : public Ling::WinBase
{
public:
	~WinBall();
	// 配置里开着才建。重复调用只建一次
	static void init();
	static void dispose();
	static bool hasBall();
	// 开关：开着就收掉并记住"关"，关着就建出来。托盘菜单与全局快捷键共用这一份，
	// 免得两处各自写一遍、将来改了一边忘了另一边
	static void toggle();
	// 拖到球上的文件，由 .cpp 里接管的那个窗口过程转进来
	void onDropFiles(HDROP drop);
private:
	WinBall();
	void onCreated() override;
	void layout() override;
	void onDown(POINT pos);
	void onMove(POINT pos);
	void onUp();
	void showMenu();
	// 松手后往最近的屏幕左右边缘靠，并把落点记进配置
	void snapAndSave();
	// 直径按当前 dpi 重算。跨屏到缩放比例不同的显示器上系统会擅自缩放窗口，得掰回来
	void applySize();
private:
	Ling::Canvas* canvas{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg, brushBorder, brushIcon;
	bool isMouseDown{ false };
	bool hasDragged{ false };
	bool dpiChanged{ false };
	POINT pressPos{ 0,0 };
};
