#pragma once
#include <windows.h>
#include <cmath>
#include <vector>

// 录制里的鼠标点击可视化：按下时在那儿扩一圈圆环再淡掉。
// MP4（VideoMp4）与 GIF（VideoGif）两条管线都要用，所以落成一份 inline 的头，
// 免得同一段逻辑在两处各自长歪
namespace ClickFx
{
	// 一个圆环活多久。半径与线宽按逻辑像素给，draw 里乘 scale 放大到物理像素 ——
	// 高 DPI 屏上画面是物理像素，不放大就是一个小点
	constexpr int lifeMs{ 420 };
	constexpr int minR{ 5 };
	constexpr int maxR{ 26 };
	// 环的颜色：亮黄压在任何底色的画面上都能看见，实心的话干扰太大，所以用环形
	constexpr COLORREF ringColor{ RGB(255, 200, 40) };

	// 帧循环里每帧调一次 pull + draw
	class Ripples
	{
	public:
		// 检测左右键的按下沿。取"此刻按住"就够了 —— 帧率 16~30fps，
		// 一次点击总要按上几十毫秒，逐帧 polling 漏不掉
		void pull()
		{
			const bool downL = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
			const bool downR = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
			POINT pt{};
			auto now = GetTickCount64();
			if ((downL && !prevL) || (downR && !prevR)) {
				GetCursorPos(&pt);
				list.push_back({ pt, now });
			}
			prevL = downL;
			prevR = downR;
			// 过期的丢掉。list 最多同时存在几个，从头扫就够
			for (auto it = list.begin(); it != list.end();) {
				if (now - it->startMs >= lifeMs) it = list.erase(it);
				else ++it;
			}
		}

		// 画到 DC 上。offX / offY 是"屏幕原点在这块画布里的位置"（取负数即画面左上角），
		// 所以两种管线只要各自把自己的左上角坐标传进来，不必关心彼此裁剪方式。
		// offX / offY 与 pos 都是物理像素；scale 只负责把半径和线宽放大
		void draw(HDC hdc, const int offX, const int offY, const float scale)
		{
			if (list.empty()) return;
			auto now = GetTickCount64();
			// GDI 的笔没有 alpha，所以用"由粗到细 + 颜色向白色插值"来近似淡出
			auto prevPen = SelectObject(hdc, GetStockObject(NULL_PEN));
			auto prevBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
			for (auto& r : list)
			{
				const auto t = (double)(now - r.startMs) / lifeMs;
				if (t < 0.0 || t > 1.0) continue;
				const int radius = (int)std::lround((minR + (maxR - minR) * t) * scale);
				const int thick = (int)std::lround((4.0 - 3.0 * t) * scale);
				if (thick < 1) continue;
				// 越接近尾声越浅：拿白色把原色调淡，省得再算一遍 RGB
				const int fade = (int)(255.0 * t);
				auto color = RGB(
					GetRValue(ringColor) + (255 - GetRValue(ringColor)) * fade / 255,
					GetGValue(ringColor) + (255 - GetGValue(ringColor)) * fade / 255,
					GetBValue(ringColor) + (255 - GetBValue(ringColor)) * fade / 255);
				HPEN pen = CreatePen(PS_SOLID, thick, color);
				if (!pen) continue;
				// pos 与 offX / offY 同属一套物理像素坐标（和光标那条路一致），不能再乘 scale
				const int x = r.pos.x + offX;
				const int y = r.pos.y + offY;
				auto prev = SelectObject(hdc, pen);
				// Ellipse 画的是外接矩形，所以左上角要减一个半径
				Ellipse(hdc, x - radius, y - radius, x + radius, y + radius);
				SelectObject(hdc, prev);
				DeleteObject(pen);
			}
			SelectObject(hdc, prevBrush);
			SelectObject(hdc, prevPen);
		}

		void clear() { list.clear(); }

	private:
		struct Item { POINT pos; ULONGLONG startMs; };
		std::vector<Item> list;
		bool prevL{ false }, prevR{ false };
	};
}
