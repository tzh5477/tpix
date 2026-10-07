#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"

// 屏幕文字。
//
// 编辑态与非编辑态用的是两套东西：
//   编辑中  —— 文本由 Canvas 上那个共用的 Ling::TextBox 画（它有自己的 swapchain，
//              光标、选区、IME 都归它），本 shape 只负责那圈虚线框；
//   编辑完 —— TextBox 隐藏，文本由本 shape 自己的 textLayout 画进 Canvas 的画布，
//              这样它才会出现在保存/复制出去的图里（导出走的是离屏 paint(ctx)）。
class ShapeText : public ShapeBase
{
public:
	ShapeText(Canvas* win);
	~ShapeText();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	// 滚轮改字号（光标停在文字上时才收得到），改完回写工具条的滑块
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	// 单击进编辑态，留不留由 finishEdit 按文本是否为空决定，这里不能提前删
	bool isValidWithoutDrag() override { return true; };
	// 收尾：把 TextBox 里的文字取回来自己画，空文本则把自己从 history 里删掉。
	// 除了本类内部，Canvas（导出图片前）和 History（删除 shape 前）也会调。
	void finishEdit();
	// 粘贴用：把内容和落点直接灌进来。样式按「文本」那一组取（见 setAttr），
	// 落点是文字的左上角（底图像素），框按文字实际尺寸先撑开 ——
	// 剪贴板来的文字随后会直接进编辑态（见 startEdit），这一步只是把该有的样子先摆好
	void setTextAt(const std::wstring& val, const float x, const float y);
	// 进编辑态：文字交给共用的 TextBox，光标落在里面。两条路会调它 ——
	// 用户点中了文字，以及粘贴（Ctrl+V 的文字直接可改，不必再"手工点一下文本组件"）
	void startEdit();
	// Canvas 只认 ShapeBase，收尾时从基类转过来走到 finishEdit
	void finishEditing() override { finishEdit(); }
	// ToolSub 上的颜色/字号/粗斜体变了，编辑中的话立即生效
	void applyStyle() override;
	// 批量旋转（见 ShapeBase::rotateBy）：位置绕 center 转 + 自身角度加 deg
	void rotateBy(const float deg, const D2D1_POINT_2F& center) override;
	// 右上角关闭按钮要用：文本的外接矩形就是那圈虚线框
	bool getShapeBounds(D2D1_RECT_F& out) const override;
	// 复制（见 ShapeBase::clone）
	bool copyable() const override { return true; }
	std::unique_ptr<ShapeBase> clone(const float dx, const float dy, Canvas* target = nullptr) const override;
protected:
	void fixupCopy() override;
	void translate(const float dx, const float dy) override;
public:
	bool isEditing{ false };
private:
	void makeTextLayout();
	// 从 ToolSub 拉一份样式并重建画刷。**一律取「文本」那一组的**，不看当前拿着哪个工具：
	// getSliderVal / getSelectedColor 给的是"当前工具"的那一档 —— 拿着矩形（线宽 2）去点一个
	// 已有的文本，字号就成了 2，文字缩成一条短横线（作者报的"文本组件会显示为-"）。
	// 现在工具栏没有「文本」这一组可读时才退回默认，见 ToolSub::getShapeSliderVal
	void setAttr();
	// 旋转中心。rect 是轴对齐的存法，画的时候才绕这个点转
	D2D1_POINT_2F center() const;
	// 按文字实际尺寸把边框盒贴合上去。滚轮改完字号后文字会溢出原来的框，得跟着长
	void fitRectToText();
private:
	std::wstring text;
	// 整块文字绕中心旋转的角度（度）。rect 本身始终轴对齐，旋转只在画的时候施加，
	// 所以导出走同一条 paint 就能得到带旋转的图，不必额外处理
	float angle{ 0.f };
	// 编辑期间把角度临时归零（见 startEdit），这个值记着退出编辑时要还原的角度
	float editAngle{ 0.f };
	// 物理像素。ToolSub::getSliderVal() 给的就是物理值，而 TextBox::setFontSize 收逻辑值，
	// 传过去时要除回 dpi。
	float fontSize{ 20.f };
	// 字体族名（DWrite 认的名字，如 Microsoft YaHei）。空串表示不指定，走 DWrite 的默认
	std::wstring fontFamily;
	bool isBold{ false }, isItalic{ false };
	UINT32 colorValue{ 0 };
	D2D1_COLOR_F color{};
	// 边框盒，窗口客户区坐标、物理像素。文字画在它内缩 borderPadding 的位置。
	D2D1_RECT_F rect{};
	Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dashedStrokeStyle;
	// 文字到虚线框的间距。取 TextBox 默认的 setPadding(6.f)，两边画出来才对得上
	float borderPadding;
	float pressX{ 0.f }, pressY{ 0.f };
	// 编辑期间挂在 TextBox 上的两个订阅。TextBox 是共用的，退出编辑必须摘掉，
	// 否则下一个 ShapeText 编辑时会把文字写进已经结束的那个里。
	winrt::event_token textChangedTok{}, focusTok{};
};

