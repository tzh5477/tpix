#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"
// 「插入图标」：FSCapture 那套小图标（鼠标指针 / 大小调整 / 信息徽章）。
// 与序号同一套用法 —— 选中图标后单击落一个、再单击再落一个，ESC 结束追加
//（WinPin::onKey 里把工具切回「几何图形」）。
//
// 图形全部用 D2D 现画（painters 表），不进 iconfont：这一批是**彩色**的
//（信息蓝 / 错误红 / 确定绿），而字体字形是单色掩码，染不上这套配色；
// 19 个字形也犯不着为它们扩一次字体。工具条按钮 / 下拉列表与图上画的是
// 同一份 painters（见 paintSample），不会两处长歪。
class ShapeIcon : public ShapeBase
{
public:
	// 图标种类的个数。下标即落盘值（toolPin.icon.kind），新增只许往表尾追加
	static constexpr int kindCount() { return 19; }
	// 在 rect 里画第 kind 号图标（全不透明、无阴影）。工具条下拉的自绘格与
	// SelectPopup 的自绘项都用它 —— 与 ShapeIcon::paint 走同一份 painters
	static void paintSample(ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect, const int kind);
	ShapeIcon(Canvas* win);
	void paint(ID2D1DeviceContext* ctx) override;
	void mouseDown(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseDrag(const float x, const float y) override;
	// 滚轮调大小；按住 Alt 滚 = 调透明度（一格 5%）。返回值语义同基类：
	// 真改到了才返回 true，调用方据此决定撤不撤刚压的那一步撤销
	bool mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	// 单击就是它的正常用法：落一枚图标，不需要拖动
	bool isValidWithoutDrag() override { return true; }
	// 外观（大小 / 透明度 / 阴影）变了，重取一遍工具条上当前那一份
	void applyStyle() override;
	// 档位（选的是哪一枚图标）变了 —— 下拉里挑图标走这条
	void applyToolStyle() override;
	// 选中框 / 迷你条 / 框选命中都要用：图标的外接方框
	bool getShapeBounds(D2D1_RECT_F& out) const override;
	bool copyable() const override { return true; }
	std::unique_ptr<ShapeBase> clone(const float dx, const float dy, Canvas* target = nullptr) const override;
	std::unique_ptr<ShapeBase> snapshot() const override;
protected:
	void fixupCopy() override;
	void translate(const float dx, const float dy) override;
private:
	// 图标落点（底图坐标，方框中心）与方框边长（物理像素）
	float cx{ 0.f }, cy{ 0.f }, size{ 0.f };
	// 拖动用的按下点（同 ShapeNumber::pressX/pressY 的用法）
	float pressX{ 0.f }, pressY{ 0.f };
	// 档位与外观。构造时从 ToolSub 取（createShape 只会在图标工具下跑到这里，
	// 面板刚把配置里的那份读进来），之后由 applyStyle / applyToolStyle 跟着工具条走
	int kind{ 0 };
	int opacity{ 100 };
	bool hasShadow{ false };
	// 整枚图标共用的画刷：painters 里现用现设色（一次 paint 要换好几种颜色），
	// 建一支复用，省得每帧每个颜色各建一支
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
};
