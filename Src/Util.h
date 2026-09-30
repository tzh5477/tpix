#pragma once
#include <include/Ling.h>
#include <fstream>
#include <array>

// 图像输出相关的工具函数。data 一律要求 BGRA、top-down、行紧凑（步长 = w*4），
// 这也是 WinPin::getImagePixels 交出来的格式。
// 剪贴板当前内容的形态。图与文都有可能同时在剪贴板上，这里按"图优先"只报一种
enum class ClipContent { None, Image, Text };

class Util
{
public:
	// 存盘格式。PNG 之外都是"有损/无 alpha"的格式，编码前按各格式的规矩处理像素
	enum class ImgFormat { Png = 0, Jpeg, WebP };
	// 同时写入 CF_DIBV5（Office / 微信 / WPS 这类原生程序认）和 "PNG" 注册格式
	//（浏览器 / Electron 程序认），两份都带 alpha
	static void saveToClipboard(const int w, const int h, BYTE* data);
	static bool saveToFile(const std::wstring& path, const int w, const int h, BYTE* data);
	// 按指定格式存盘。quality 只给 JPEG / WebP 用（0–100），PNG 忽略它
	static bool saveToFile(const std::wstring& path, const int w, const int h, BYTE* data,
		const ImgFormat format, const float quality = 90.f);
	static std::wstring getExtOfFormat(const ImgFormat format);
	static int getSaveFormat();
	// 弹系统另存为对话框，返回空串表示用户取消
	static std::wstring getSaveFilePath(HWND hwnd, const std::wstring& ext = L"png");
	// 决定这一次存到哪：开了自动保存就直接按目录 + 模板算出路径（不弹窗），
	// 否则弹另存为对话框。返回空串表示用户取消
	static std::wstring resolveSavePath(HWND hwnd);
	// 按模板生成文件名（不含目录）。支持 %y %m %d %H %M %S（补零）、%n（三位序号）、%%。
	// 模板里一个占位符都没有时退化成时间戳，避免产出空文件名
	static std::wstring formatFileName(const std::wstring& tpl, const std::wstring& ext);
	// 以当前时间生成默认文件名，精确到毫秒，避免连续保存时重名
	static std::wstring createFileName(const std::wstring& ext);
	// GDI 抓屏。返回 BGRA、top-down、行紧凑（步长 = w*4），与本类其他函数的入参格式一致
	static std::vector<BYTE> captureScreen(const int x, const int y, const int w, const int h);
	// 把文件路径以 CF_HDROP 写进剪切板，粘贴到资源管理器/聊天窗口就是一个文件
	static void addFileToClipboard(const std::wstring& filePath);
	// 把图存成缓存文件，再交给外部插件 ImageReader.exe 做文字识别。插件先在本 exe
	// 同目录找，再找 %appdata%\ScreenCapture\plugin，都找不到就用默认浏览器打开它的
	// release 页面让用户自己下。缓存图由插件读完后自己删。
	static bool openWithImageReader(const int w, const int h, BYTE* data);
	// 用 quirc 识别图里的二维码，返回识别到的内容，没识别到返回空串。
	// 图里有多个码时用换行拼在一起
	static std::wstring decodeQrCode(const int w, const int h, BYTE* data);
	// 缩放 BGRA 图。缩略图用，块平均而不是最近邻 —— 最近邻在小图上会出摩尔纹。
	// 入参不合法返回 false，dst 不动
	static bool resizeBGRA(const int srcW, const int srcH, BYTE* srcData,
		const int dstW, const int dstH, std::vector<BYTE>& dstData);
	// 解码图片文件成 BGRA top-down 行紧凑。格式由 WIC 自己认（png/jpg/webp/bmp/gif 都行），
	// 认不出来返回 false
	static bool loadImageBytes(const std::wstring& path, std::vector<BYTE>& out, DWORD& w, DWORD& h);
	// 读剪贴板：有图就出图（BGRA top-down），没图有文字就出文字。
	// 图片优先取 PNG（浏览器和不少现代程序放的就是它，alpha 保得住），
	// 回退 CF_DIBV5 / CF_DIB——这两种给的是 DIB，只认 24/32bpp 的 BI_RGB 与 BI_BITFIELDS
	static ClipContent readClipboard(std::vector<BYTE>& img, int& w, int& h, std::wstring& text);
	// 同上，但源是内存里的一段编码数据（剪贴板上的 PNG 就是这种）
	static bool decodeImageBytes(BYTE* buf, DWORD size, std::vector<BYTE>& out, DWORD& w, DWORD& h);
};
