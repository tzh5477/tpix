#pragma once
#include <include/Ling.h>
#include <filesystem>
#include <utility>
#include <vector>
#include <winrt/Windows.Data.Json.h>
using namespace winrt::Windows::Data::Json;

class Setting
{
public:
	~Setting();
	static void init();
	// 必须在 CoUninitialize 之前调用：configObj 是 WinRT 对象，晚一步释放就是野内存
	static void dispose();
	static Setting* get();
	std::filesystem::path getDataPath();
	const JsonObject getConfigObj();
	void setShortcutKey(const std::wstring& type, const std::vector<std::wstring>& keys);
	std::wstring getShortcutKey(const std::wstring& type);
	void setAutoStart(bool autoStart);
	bool getAutoStart();
	std::wstring getLang();
	void setLang(const std::wstring& lang);
	void initShortcutKeys();
	// 贴图窗口子工具栏（ToolSub）的状态。每个工具在 config.json 的 toolPin 下各占一组，
	// 组名就是 ToolMain 上的按钮 id（rect / ellipse / ... / eraser），键名由调用方给
	// （fill、width、colorIndex 之类，各工具语义不同）。
	// 取不到就返回 def —— 老版本的配置文件里没有这些键，用户手工改坏了也算取不到，都不该抛异常。
	// set 一律立即落盘：用户调一次工具状态就得记住一次。
	bool getToolFlag(const std::wstring& tool, const std::wstring& key, bool def);
	void setToolFlag(const std::wstring& tool, const std::wstring& key, bool val);
	float getToolNum(const std::wstring& tool, const std::wstring& key, float def);
	void setToolNum(const std::wstring& tool, const std::wstring& key, float val);
	// 工具面板上的文本项（水印文字这类）。同一张 getToolObj 表，只是取的是字符串
	std::wstring getToolStr(const std::wstring& tool, const std::wstring& key, const std::wstring& def);
	void setToolStr(const std::wstring& tool, const std::wstring& key, const std::wstring& val);
	// 输出与自动保存。存在 config.json 的 save 组里：
	// format（0=PNG / 1=JPEG / 2=WebP）、auto（自动保存，不弹另存为）、
	// dir（自动保存目录，空串表示走每次弹窗时用户选的那个目录所在的数据目录）、
	// nameTpl（文件名模板，见 Util::formatFileName）
	int getSaveFormat();
	void setSaveFormat(int val);
	bool getAutoSave();
	void setAutoSave(bool val);
	std::wstring getSaveDir();
	void setSaveDir(const std::wstring& dir);
	std::wstring getSaveNameTpl();
	void setSaveNameTpl(const std::wstring& tpl);
	// 复制到剪贴板之后，自动把焦点还给截图前那个窗口并粘进去（H3 后半）。
	// 默认关：它会把焦点从 tpix 挪走，还会往别人的窗口里塞东西，不该静默生效
	bool getAutoPaste();
	void setAutoPaste(bool val);
	// 历史（截图 + 剪贴板）最多留多少条。超了从最旧那条开始删，连文件一起删
	int getHistoryLimit();
	void setHistoryLimit(int val);
	// 是否记录剪贴板历史。关掉之后监听还挂着（省得重建窗口），只是不再往库里写
	bool getClipboardHistory();
	void setClipboardHistory(bool val);
	// 捕获。存在 config.json 的 cap 组里：delay（延时秒数）、autoShot / autoShotMin
	//（定时自动截图的开关与间隔分钟）、cursor（截图里带上鼠标指针）
	int getCapDelay();
	void setCapDelay(int val);
	bool getAutoShot();
	void setAutoShot(bool val);
	int getAutoShotMin();
	void setAutoShotMin(int val);
	bool getIncludeCursor();
	void setIncludeCursor(bool val);
	// 框选形状：0 = 矩形，1 = 手绘自由多边形（多边形以外的像素导出成透明）
	int getCapShape();
	void setCapShape(int val);
	// 固定尺寸区域：返回全局预设表里的第几个，0 是不固定。尺寸是截图要截到的像素
	static const std::vector<std::pair<int, int>>& fixedSizePresets();
	// 越界的下标按"不固定"对待，老配置 / 手改过的配置文件不至于把框选搞废
	int getCapFixedIdx();
	void setCapFixedIdx(int val);
	// 第 idx 档的宽高，写到出参里；不固定（或 idx 越界）返回 false
	static bool fixedSize(int idx, int& w, int& h);
	// 录屏时鼠标点击可视化（按下处扩一圈圆环）。默认关：它会把画面改掉，不该静默生效
	bool getClickFx();
	void setClickFx(bool val);
	// 全局鼠标（C5 余项）：按住 Win 键拖动直接出结果 —— 左键贴图 / 中键复制 / 右键认文字。
	// 默认关：它要吞掉 Win+拖动那一串鼠标消息，属于抢系统手势，不该静默生效
	bool getGlobalMouse();
	void setGlobalMouse(bool val);
	// 滚动截图的方向：true = 横向（拼出来的图往右长），false = 竖向（默认）
	bool getLongHorizontal();
	void setLongHorizontal(bool val);
	// 上次检查更新是哪一天（std::chrono::days 的计数，即 1970-01-01 以来的天数），
	// 从来没查过返回 0。一天最多查一次服务端，靠它记账 —— 每次空闲都去请求纯属浪费人家的流量
	long long getUpdateCheckDay();
	void setUpdateCheckDay(long long day);
	// 贴图持久化。数组里每项是一张贴图的落点 / 尺寸 / 属性，图片文件在数据目录 pin/ 下。
	// 序列化由 WinPin 自己做，这里只管存取
	JsonArray getPins();
	void setPins(const JsonArray& arr);
	bool getRestorePins();
	void setRestorePins(bool val);
	// 配置导入导出（H1 后半）。导出的是整份 config.json，但**不带 pin 那一组** ——
	// 贴图是运行时状态，图片文件躺在数据目录的 pin/ 下，跟着配置一起搬到别的机器上只会指空。
	// 导入则是整份替换：解析不出来就返回 false，configObj 一个字都不动
	bool exportConfig(const std::wstring& path) const;
	bool importConfig(const std::wstring& path);
private:
	// 按当前配置把热键重新注册一遍。导入后要调它，因为热键是"写进系统里"的那一类设置，
	// 换了配置就得按新的重来。注意别用 initShortcutKeys 顶替：它还会再挂一次
	// onHotKey / onSecondInstance 回调，一个快捷键会被响应两次
	void applyShortcutKeys();
	Setting();
	// toolPin.<tool> 那个 JsonObject。缺哪一层就现建一层挂上去 ——
	// SetNamedValue 得有个落脚的对象，而这两层在旧配置文件里都不存在
	JsonObject getToolObj(const std::wstring& tool);
	// save 那一组，缺则现建一层挂上去（理由同 getToolObj）
	JsonObject getSaveObj();
	// pin 那一组（贴图持久化），缺则现建
	JsonObject getPinObj();
	// cap 那一组（捕获：延时 / 定时 / 指针），缺则现建
	JsonObject getCapObj();
	std::filesystem::path initDataPath();
	// 决定配置文件用哪一份：exe 同目录有 config.json 就用它（绿色版，配置跟着程序走），
	// 否则用 %appdata%\tpix\config.json。二者只认一个，读哪儿就写哪儿。
	std::filesystem::path initConfigPath();
	void save();
private:
	const std::filesystem::path dataPath;
	// 必须声明在 dataPath 之后：initConfigPath 找不到 exe 同目录的配置时要回落到 dataPath 上，
	// 成员按声明顺序初始化
	const std::filesystem::path configPath;
	JsonObject configObj;
};

