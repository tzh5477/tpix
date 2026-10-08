#pragma once
#include <include/Ling.h>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include <winrt/Windows.Data.Json.h>
using namespace winrt::Windows::Data::Json;

// 一个大模型接口（OpenAI 兼容服务）。「自定义接口名称」就是 name —— 设置页的下拉框、
// 对话窗的模型切换框都拿它在界面上代表这一项；id 只是它的稳定键，改名、改地址都不动它，
// 否则已经配好的那些场景绑定会跟着失效
struct AiProvider
{
	std::wstring id;
	std::wstring name;
	std::wstring baseUrl;
	std::wstring apiKey;
	std::wstring model;                 // 当前选中的模型
	std::vector<std::wstring> models;   // 上次拉到的列表，填下拉框用
};

// 一个「业务场景」解出来的、可以直接发请求的那几个值。
// 需求要求不同场景能配不同的组合（翻译走 A 接口 A 模型，表格识别走 B 接口 X 模型），
// 所以配置里存的是"选了哪个接口 + 哪个模型"，真正发请求之前要再解一层。
// ok = false 表示这一整套没配齐（没有接口、或它没填模型），调用方应当引导去设置
struct AiCred
{
	bool ok{ false };
	std::wstring providerId;
	std::wstring providerName;
	std::wstring baseUrl;
	std::wstring apiKey;
	std::wstring model;
};

// 四类业务用途的键名。加一个场景要同步两处：这里一行 + WinSettingAi 里那张场景行表
namespace AiScenario
{
	inline constexpr std::wstring_view chat{ L"chat" };            // AI 对话
	inline constexpr std::wstring_view translate{ L"translate" };  // 翻译（走大模型那条路）
	inline constexpr std::wstring_view recognize{ L"recognize" };  // 图片里的文字
	inline constexpr std::wstring_view table{ L"table" };          // 图片里的表格
}

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
	// 设置页"按一下键盘来录快捷键"期间必须把全局热键全摘掉：按下的组合若正好是已注册的
	// 那一个，Windows 只把修饰键送进窗口、把那一下"主键"吞掉换成 WM_HOTKEY（实测见
	// 2026-10-07 的 hkprobe 探针），设置页于是什么也录不到，还会顺带把那个动作触发一次。
	// on=false 时按当前配置重新注册一遍
	void setShortcutCapture(bool on);
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
	// AI（翻译 / 对话，S1–S3）。存在 config.json 的 ai 组。
	// ⚠️ apiKey 是明文落的：本机单人工具，加密换不来什么，反倒会让已有的「导出配置」跑到别的机器上就用不了。
	//    补偿靠 exportConfig 把它摘出去，见那里的注释。
	//    这一组现在只放"与接口无关"的那几个键（翻译选哪家、火山 AK/SK、翻译语言、历史策略）
	std::wstring getAiStr(const std::wstring& key, const std::wstring& def);
	void setAiStr(const std::wstring& key, const std::wstring& val);
	// ---- 大模型接口（可多个，各带一个自定义名称）----
	// 放在 ai.providers 数组里，每项是一个 AiProvider。老版本那份扁平的
	// baseUrl / apiKey / model / models 会在读到它们的时候搬进来变成第 0 项（见 ensureProviders）
	std::vector<AiProvider> getAiProviders();
	// 取一个接口。找不到返回 false —— 它被删掉了，或者配置文件被手写坏了
	bool getAiProvider(const std::wstring& id, AiProvider& out);
	// 更新同 id 的那一项，没有就追加到末尾。改一次就落盘
	void setAiProvider(const AiProvider& provider);
	void removeAiProvider(const std::wstring& id);
	// 给「新增接口」生成一个还没被占用的 id
	std::wstring newProviderId();
	// 接口在界面上的显示名。名称是用户自己填的、允许为空 —— 那时给一个语言包里的默认名，
	// 好让用户知道这一项是什么。由 UI 层调用（这时语言包一定已经起来了）
	static std::wstring providerName(const AiProvider& provider);
	// ---- 场景 ->（接口, 模型）----
	// 不同业务各用各的一套：翻译可以走 A 接口 A 模型，表格识别走 B 接口 X 模型。
	// 这两个返回值都已兜底：没配过就退到第 0 个接口（以及它当前那个模型），
	// 所以调用方可以直接拿去用，不必先判断"有没有配过"
	std::wstring getScenarioProvider(const std::wstring& scenario);
	std::wstring getScenarioModel(const std::wstring& scenario);
	void setScenario(const std::wstring& scenario, const std::wstring& providerId,
		const std::wstring& model);
	// 把一个场景解成能直接发请求的那三个值。缺一样就 ok = false（三个字符串仍会尽量填上，
	// 好让界面把"到底缺了哪一样"指出来）
	AiCred credFor(const std::wstring& scenario);
	// 对话历史（AiHistory）。三个键决定了落盘与清理：
	// historySave 关掉之后不记也不读，且下次写盘时把已有文件删掉 —— 磁盘上不留聊天记录；
	// historyLimit 是最多留几个会话，historyDays 是留最近多少天。两个维度各防一段，见 AiHistory::trim
	bool getAiHistorySave();
	void setAiHistorySave(bool val);
	int getAiHistoryLimit();
	void setAiHistoryLimit(int val);
	int getAiHistoryDays();
	void setAiHistoryDays(int val);
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
	// ai 那一组（接口列表 / 场景绑定 / 翻译选哪家 / 历史策略），缺则现建
	JsonObject getAiObj();
	// ai.providers 数组，缺则现建
	JsonArray getAiProvidersArray();
	// ai.scenarios 对象（场景名 -> {provider, model}），缺则现建
	JsonObject getScenarioObj();
	// 老版本的配置把 baseUrl / apiKey / model / models 平铺在 ai 组里。读到它们就把它们
	// 搬成第 0 个接口 —— 用户已经填好的地址与密钥不该因为升了版本就"变成没配过"。
	// 顺带保证至少有一个接口：界面上总得有一行可以编辑
	void ensureProviders();
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

