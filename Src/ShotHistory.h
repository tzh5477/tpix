#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <Windows.h>

// 截图历史与剪贴板历史。两者共用一套存储（数据目录 history/）和一套 UI，
// 只在 source 上区分：Shot 是本工具的截图，Clipboard 是从系统剪贴板监听来的。
//
// 图片条目落两张文件：原图 <id>.png 与缩略图 <id>_t.png。缩略图是给列表用的 ——
// 历史可能有几百条，列表每次都去解一张 4K 原图会卡住。
// 索引是 history/index.json，进程启动时读一次，之后每次增删都整体写回。
class ShotHistory
{
public:
	enum class Source { Shot = 0, Clipboard = 1 };
	struct Item
	{
		std::wstring id;
		Source source{ Source::Shot };
		long long time{ 0 };   // 毫秒级 Unix 时间戳，排序与显示都用它
		bool isText{ false };
		std::wstring text;     // 文本条目的内容
		std::wstring file;     // 图片文件名（相对 history 目录），文本条目为空
		std::wstring thumb;    // 缩略图文件名
		int w{ 0 }, h{ 0 };
	};
public:
	static void init();
	static void dispose();
	static ShotHistory* get();
	// 记一张图。data 是 BGRA top-down，与 Util 里其他函数同一套格式
	void addImage(Source source, const int w, const int h, BYTE* data);
	void addText(Source source, const std::wstring& text);
	void removeById(const std::wstring& id);
	// 清空某一类。另一类不动 —— 用户只想清剪贴板时不该连截图一起没
	void clear(Source source);
	// 排序后的列表：时间新的在前
	std::vector<Item> list(Source source) const;
	// 条目的绝对路径。文本条目返回空串
	std::wstring imagePath(const Item& item) const;
	std::wstring thumbPath(const Item& item) const;
	// 把历史图片读回来（BGRA top-down），贴图用。失败返回 false
	bool loadImage(const Item& item, std::vector<BYTE>& data, int& w, int& h);
	// 本工具自己往剪贴板写内容之后调一次：紧接着那次 WM_CLIPBOARDUPDATE 不入库，
	// 否则"截图 -> 复制到剪贴板"会在剪贴板历史里多出一条一模一样的图
	void skipNextClipboard();
	// 剪贴板变了。监听窗口的 WndProc 是普通函数，只能从这里进来
	void onClipboardUpdate();
private:
	ShotHistory();
	~ShotHistory();
	// 唯一持有者是本类的静态 unique_ptr，允许它析构，别处仍然不能 delete
	friend struct std::default_delete<ShotHistory>;
	void load();
	void save();
	// 超过上限就从最旧那条开始删，连文件一起删
	void trim(Source source);
	void removeFiles(const Item& item);
	// 建一个 message-only 窗口专门收 WM_CLIPBOARDUPDATE。它是 OS 回调里唯一需要
	// 窗口句柄的入口，而 Ling 的托盘消息窗口是框架私有的，拿不到句柄
	void initClipboardListener();
private:
	std::vector<Item> items;
	std::filesystem::path dir;
	HWND clipHwnd{ nullptr };
	bool skipOnce{ false };
	// 同一毫秒内可能连着来两条（复制图片和复制文本几乎同时发生），
	// 靠它把文件名岔开
	int seq{ 0 };
};
