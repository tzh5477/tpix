#pragma once
#include <string>

// 贴图窗口除了"截一张图"之外的来源入口：剪贴板、图片文件、一段文字、一个颜色值。
// 几个入口最后都合成一块 BGRA 像素，交给 WinPin::initFromData —— 贴图窗口本身
// 只认像素，不关心它是从哪来的。
class PinSource
{
public:
	// 剪贴板上有图就贴图；有文字时，看着像颜色值（#RGB / #RRGGBB / #AARRGGBB）就贴一块色，
	// 否则把文字渲染成一张图贴出来。什么都没有就什么都不做
	static void fromClipboard();
	// 弹打开文件对话框，选中的图片贴出来
	static void fromFile(HWND hwnd);
	// 直接按路径贴图（悬浮球拖放走的就是它）。不是图片就什么都不做
	static void fromPath(const std::wstring& path);
	// 一段文字渲染成一张图贴出来。空串什么都不做
	static void fromText(const std::wstring& text);
	// 按扩展名判断一个路径是不是纯文本文件（.txt/.md/.json/.cpp…）
	static bool isTextFile(const std::wstring& path);
	// 文本文件：探测编码（BOM / UTF-8 / 系统代码页）读成 wstring，开一扇可编辑文本钉窗。读不出就什么都不做
	static void fromTextFile(const std::wstring& path);
	// 颜色值渲染成一块纯色图贴出来。认不出来（不是 # 开头的十六进制）就什么都不做
	static void fromColor(const std::wstring& color);
	// 从截图历史里贴图：连着按 = 依次把第 2、第 3… 新的那张贴出来，停手两秒重新从第 2 张数
	static void pinNextOlder();
	// 同上，换成剪贴板历史。文本条目现渲染成一张图贴出来（历史里存的是字，不是像素）
	static void pinNextOlderClip();
};
