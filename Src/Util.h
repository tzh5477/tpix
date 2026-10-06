#pragma once
#include <include/Ling.h>
#include <fstream>
#include <array>

struct IWICBitmapFrameDecode;   // 前向声明就够：只在实现里用到它的成员，这里不必拖进 wincodec.h

// 图像输出相关的工具函数。data 一律要求 BGRA、top-down、行紧凑（步长 = w*4），
// 这也是 WinPin::getImagePixels 交出来的格式。
class Util
{
public:
	// 剪贴板当前内容的形态。图与文都有可能同时在剪贴板上，这里按"图优先"只报一种。
	// 放在类里：调用方都用 Util::ClipContent 限定名取它
	enum class ClipContent { None, Image, Text };
	// 存盘格式。PNG 之外都是"有损/无 alpha"的格式，编码前按各格式的规矩处理像素
	enum class ImgFormat { Png = 0, Jpeg, WebP };
	// 同时写入 CF_DIBV5（Office / 微信 / WPS 这类原生程序认）和 "PNG" 注册格式
	//（浏览器 / Electron 程序认），两份都带 alpha
	static void saveToClipboard(const int w, const int h, BYTE* data);
	static bool saveToFile(const std::wstring& path, const int w, const int h, BYTE* data);
	// 按指定格式存盘。quality 只给 JPEG / WebP 用（0–100），PNG 忽略它
	static bool saveToFile(const std::wstring& path, const int w, const int h, BYTE* data,
		const ImgFormat format, const float quality = 90.f);
	// 同上，但编到内存里而不是落盘。给"要的是字节本身"的场合用（发给大模型时的 base64）。
	// 默认 PNG：表格截图这类要拿去认字的图不能先过一道有损压缩
	static bool encodeImageBytes(const int w, const int h, const BYTE* data, std::vector<BYTE>& out,
		const ImgFormat format = ImgFormat::Png, const float quality = 90.f);
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
	// 自动保存那套目录 + 模板算出来的路径，不弹窗。定时自动截图这种没人盯着的时候走它
	static std::wstring autoSavePath();
	// GDI 抓屏。返回 BGRA、top-down、行紧凑（步长 = w*4），与本类其他函数的入参格式一致。
	// withCursor 为真时把鼠标指针画上去（指针位置与形状取自 snapshotCursor 的快照，
	// 没快照就现取一个）—— 滚动截图那一帧帧的拼接不该带指针，所以默认不画
	static std::vector<BYTE> captureScreen(const int x, const int y, const int w, const int h,
		bool withCursor = false);
	// 记下"此刻"鼠标指针的形状与位置。必须在截图窗口建起来之前调：
	// 窗口一出来指针就换成 tpix 自己的了，那时再取，截到的是我们的箭头
	static void snapshotCursor();
	// 把文件路径以 CF_HDROP 写进剪切板，粘贴到资源管理器/聊天窗口就是一个文件
	static void addFileToClipboard(const std::wstring& filePath);
	// 同时放两份上剪贴板：CF_HTML（Word / Excel 认，粘出来是一张真表）和纯文本
	//（记事本这类只认后者）。text 一般给 html 里那些内容的 tab 分隔版，两厢对照着填
	static void setHtmlToClipboard(const std::wstring& html, const std::wstring& text);
	// 记下"此刻"前台那个窗口，自动粘贴要把焦点还给它。自己的窗口、桌面、任务栏都不算 ——
	// 焦点还到这些地方等于什么都没做，这类一律记成 nullptr
	static HWND snapshotForeground();
	// 把焦点还给 hwnd，再发一次 Ctrl+V（把剪贴板上的东西粘进去）。
	// 换焦点之后要等那边真的激活才发键，所以这里最多会堵上 0.2 秒左右
	static void pasteToWindow(HWND hwnd);
	// 把图存成缓存文件，再交给外部插件 ImageReader.exe 做文字识别。插件先在本 exe
	// 同目录找，再找 %appdata%\tpix\plugin，都找不到就用默认浏览器打开它的
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
	// 把 WIC 已经解出来的一帧转成 BGRA 行紧凑。动图逐帧解码用得上（见 AnimImage），
	// 静态图走上面两个入口，它们内部取的也是第 0 帧
	static bool decodeWicFrame(IWICBitmapFrameDecode* frame, std::vector<BYTE>& out, DWORD& w, DWORD& h);
};
