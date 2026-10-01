#pragma once
#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>
#include <mutex>

// 录制里的按键显示：把最近按下的键（含 Ctrl / Shift / Alt / Win 组合）叠在画面上。
// 与 ClickFx 同族，但管的是键盘。跨线程：钩子回调在装它的那条线程上跑（UI 线程），
// 画到帧上是录制线程，两边靠一把锁换数据
namespace KeyFx
{
	// 一个按键显示多久
	constexpr int lifeMs{ 1500 };

	class Tracker
	{
	public:
		// 低级键盘钩子里调。vk 是虚拟键码，down 是按下还是抬起
		void onKey(DWORD vk, const bool down)
		{
			std::lock_guard lock{ mtx };
			if (down) {
				// 自动重复的 keydown 会连着来，只记第一次
				if (std::find(pressed.begin(), pressed.end(), vk) == pressed.end()) {
					pressed.push_back(vk);
					chord.push_back(vk); // 整个手势的并集，松手时拿它定格
				}
				// 按住期间先显示当前按住的组合（单按住 Ctrl 也能看见）
				text = makeText(pressed);
				atMs = GetTickCount64();
				return;
			}
			auto it = std::find(pressed.begin(), pressed.end(), vk);
			// 没记过这个键的按下（录制开始前就按着、或漏了 keydown），
			// 就别凭空定格出一个幽灵键名
			if (it == pressed.end()) return;
			pressed.erase(it);
			// 组合还没散，保持按住那一刻的定格
			if (!pressed.empty()) return;
			// 全部松手才算一次完整按键。只取 pressed 的话，最后松手的那个修饰键
			// 会把 "Ctrl + C" 冲掉成 "Ctrl" —— 所以定格用 chord 而不是 pressed
			text = makeText(chord);
			chord.clear();
			atMs = GetTickCount64();
		}

		// 取当前该显示的文本。没有存活的按键时返回 false
		bool current(std::wstring& out, const ULONGLONG now) const
		{
			std::lock_guard lock{ mtx };
			if (text.empty() || now - atMs >= lifeMs) return false;
			out = text;
			return true;
		}

		void clear()
		{
			std::lock_guard lock{ mtx };
			pressed.clear();
			chord.clear();
			text.clear();
		}

	private:
		// 虚拟键码 -> 短名。只覆盖会去按的那几十个，认不出的就按 vk 打出来
		static std::wstring nameOf(const DWORD vk)
		{
			switch (vk) {
			case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return L"Ctrl";
			case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return L"Shift";
			case VK_MENU: case VK_LMENU: case VK_RMENU: return L"Alt";
			case VK_LWIN: case VK_RWIN: return L"Win";
			case VK_RETURN: return L"Enter";
			case VK_SPACE: return L"Space";
			case VK_BACK: return L"Backspace";
			case VK_TAB: return L"Tab";
			case VK_ESCAPE: return L"Esc";
			case VK_DELETE: return L"Del";
			case VK_LEFT: return L"\u2190";
			case VK_RIGHT: return L"\u2192";
			case VK_UP: return L"\u2191";
			case VK_DOWN: return L"\u2193";
			case VK_PRIOR: return L"PgUp";
			case VK_NEXT: return L"PgDn";
			case VK_HOME: return L"Home";
			case VK_END: return L"End";
			case VK_INSERT: return L"Ins";
			case VK_SNAPSHOT: return L"PrtSc";
			default: break;
			}
			// F1 - F24
			if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
			// 主键盘数字与字母。布局相关的符号位一概不猜，直接给键码名
			if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
				return std::wstring(1, (wchar_t)vk);
			}
			if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
				return L"Num" + std::to_wstring(vk - VK_NUMPAD0);
			}
			return L"#" + std::to_wstring(vk);
		}

		static std::wstring makeText(const std::vector<DWORD>& keys)
		{
			std::wstring s;
			for (auto vk : keys) {
				if (!s.empty()) s += L" + ";
				s += nameOf(vk);
			}
			return s;
		}

		mutable std::mutex mtx;
		std::vector<DWORD> pressed; // 此刻还按着的
		std::vector<DWORD> chord;   // 这一按从头到尾碰过的所有键，松干净时用来定格
		std::wstring text;
		ULONGLONG atMs{ 0 };
	};

	// 画一次：一块圆角底 + 白字，摆在 (cx, bottom) 的正上方居中。
	// scale 是 dpi，字与留白都跟着放大；位置本身是画面像素，不受 dpi 影响。
	// 字高自己按 scale 建，不依赖调用方 dc 上挂着什么字体
	inline void draw(HDC hdc, const std::wstring& text, const int cx, const int bottom, const float scale)
	{
		if (text.empty()) return;
		// GIF 一路用的是跨帧复用的内存 dc，改了 bk/文字色不还原会漏到下一帧
		const int saved = SaveDC(hdc);
		if (!saved) return;
		auto font = CreateFontW((int)(14 * scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
			DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
		if (font) SelectObject(hdc, font);
		RECT rt{ 0, 0, 0, 0 };
		DrawTextW(hdc, text.data(), (int)text.size(), &rt, DT_CALCRECT | DT_SINGLELINE);
		const int w = rt.right - rt.left;
		const int h = rt.bottom - rt.top;
		const int pad = (int)(8 * scale);
		const int bw = w + pad * 2;
		const int bh = h + pad * 2;
		int left = cx - bw / 2;
		int top = bottom - bh - (int)(12 * scale);
		// 区域很小时上面/左边会算成负数，整块被裁掉。贴到边上总比看不见强
		if (left < 0) left = 0;
		if (top < 0) top = 0;
		// 不透明的深底。GDI 画刷没有 alpha，为一块只出现一秒多的牌子去
		// 走 AlphaBlend 不划算
		auto brush = CreateSolidBrush(RGB(0, 0, 0));
		if (brush) SelectObject(hdc, brush);
		SelectObject(hdc, GetStockObject(NULL_PEN));
		RoundRect(hdc, left, top, left + bw, top + bh, (int)(6 * scale), (int)(6 * scale));
		SetBkMode(hdc, TRANSPARENT);
		SetTextColor(hdc, RGB(255, 255, 255));
		RECT textRect{ left + pad, top + pad, left + pad + w, top + pad + h };
		DrawTextW(hdc, text.data(), (int)text.size(), &textRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
		// 先还原再删：还选在 dc 上的对象 DeleteObject 是删不掉的
		RestoreDC(hdc, saved);
		if (font) DeleteObject(font);
		if (brush) DeleteObject(brush);
	}
}
