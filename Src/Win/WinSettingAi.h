#pragma once
#include <include/Ling.h>
#include <functional>
#include <string>
#include <vector>
// LLM 与翻译两页设置共用同一套「一行 + 控件」的搭法，所以留在同一个类里，由设置窗口
// 分别调 buildLlm / buildTrans 建自己那一半。构造函数刻意不建任何东西 ——
// makeChild<T>() 只会 new T(win)，没有地方塞"建哪一半"的参数
class WinSettingAi :public Ling::Node
{
public:
	WinSettingAi(Ling::WinBase* parent);
	~WinSettingAi();
	// 菜单「LLM设置」：服务地址 / 密钥 / 模型 + 连接验证 + 历史落盘策略
	void buildLlm();
	// 菜单「翻译设置」：翻译服务选择 + 火山 AK / SK + 连接验证
	void buildTrans();
private:
	// AI（S1 云端对话）：服务地址 / 密钥 / 模型（可下拉选也可直接输）+ 连接验证 +
	// 历史落盘的两档清理策略
	void initAiCtrls();
	// 翻译（S2/S3）。与 AI 那组分开摆：两组的凭据不是一家的，混在一起容易填错地方
	void initTransCtrls();
	// 一行「标签 + 控件」。生成的行节点作为返回值交给调用方塞控件，分隔线是本节点的
	// 子节点而不是行内的，必须在下一行入列之前加好，所以顺手在这里加掉
	Ling::Node* makeRow(const std::wstring& labelKey);
	// 一行里那个「点一下弹出全部选项」的按钮。按钮上显示当前这一档，
	// 选完由 onPick 落盘（按钮上的字这里自己换掉）。
	// 原来这些位置都是「点一次切一档」的循环按钮，档位一多就得点好几下才转到想要的那个
	Ling::Button* makeSelectBtn(Ling::Node* row, float width,
		const std::vector<std::wstring>& items, int cur,
		std::function<void(int)> onPick);
	// 行尾那个开 / 关开关：按钮上是勾或叉，点一下弹出两项直接选。
	// read 取当前是否开着，write 把新状态落盘（顺带做装钩子这类副作用）
	Ling::Button* makeSwitchBtn(Ling::Node* row,
		std::function<bool()> read, std::function<void(bool)> write);
	// 验证连接那个请求还在飞、页面却已经被换掉或窗口已关时，回调不能再碰那几个节点
	// —— postDone 是"取消也照送"的语义，光 cancel 挡不住它。切菜单会连 content 一起重建，
	// 所以这里的析构就是唯一的尽头
	std::shared_ptr<bool> aiAlive{ std::make_shared<bool>(true) };
};
