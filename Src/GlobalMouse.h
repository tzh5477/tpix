#pragma once

// PixPin 那套「全局鼠标」：按住 Win 键拖一下直接出结果，不必先走截图窗口。
//   Win + 左键拖动 = 把框住的那一块贴到屏幕上
//   Win + 中键拖动 = 复制到剪贴板
//   Win + 右键拖动 = 认出框里的文字并复制到剪贴板
// 认手势靠一个 WH_MOUSE_LL 钩子；框选期间那一串鼠标消息全部吞掉 —— 否则目标窗口
// 会顺着同一次拖动选中一片文字、拖歪一个滑块。
class GlobalMouse
{
public:
	// 开关。关着的时候一个钩子都不挂：全局钩子要旁听系统里每一次鼠标消息，
	// 用不上它就不该挂着
	static void setEnabled(bool on);
	static void dispose();
};
