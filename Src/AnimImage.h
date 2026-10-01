#pragma once
#include <Windows.h>
#include <string>
#include <vector>

// 动图（GIF，以及 WIC 认得的其他多帧格式）解码出来的一帧。
// pixels 是 BGRA、top-down、行紧凑，与贴图窗口的底图同一套布局；
// 每一帧都已按 GIF 的帧间规则合成过（见 AnimImage::load），可以直接顺序播放
struct AnimFrame
{
	std::vector<BYTE> pixels;
	UINT w{ 0 }, h{ 0 };
	// 这一帧停留多久（毫秒）
	UINT delayMs{ 100 };
};

class AnimImage
{
public:
	// 解出全部帧。静态图（只有一帧）或解码失败返回 false —— 调用方按普通静态图处理
	static bool load(const std::wstring& path, std::vector<AnimFrame>& frames);
};
