#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"
class ShapeNumber : public ShapeBase
{
public:
	enum class NumStyle { Arabic = 0, AlphaLower, AlphaUpper, Roman, Chinese };
	// 外圈样式。Circle / Square 是 pixpin 那种纯粹的圈号（没有尾巴），是默认的两项；
	// 带尾巴的"指向某处"版本单独留成带 Arrow 的两个。
	// 顺序不能随便改：这个值直接落盘（config.json 的 toolPin.number.ringStyle），
	// 插到中间会让老配置串味 —— 所以新增的两个排在 None 后面
	enum class RingStyle { Circle = 0, Square, None, CircleArrow, SquareArrow };
	// preview 为 true 时是 hover 预览用的临时实例：它不进 history、也不从工具条"领号"
	// （见 WinPin::numberPreview），免得一个还没落下的编号把计数推着走
	ShapeNumber(Canvas* win, bool preview = false);
	~ShapeNumber();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	// 选中态下的按键：+/- 改编号并顺移，F2 编辑追加的描述文本
	void onKey(UINT key) override;
	// 鼠标还没落笔时在 (x,y) 处画一个"将要落下的编号"，样式取工具条当前那一份。
	// val 由调用方从 ToolSub 现取（取一次不推进计数的那种）
	void previewAt(const float x, const float y, const int previewVal);
	// 单击就是它的正常用法：落一个序号徽章，不需要拖动
	bool isValidWithoutDrag() override { return true; };
	// ToolSub 上的编号样式 / 外圈样式变了，重排自己的几何与文字
	void applyStyle() override;
	// 退出编辑并把 TextBox 里的文字收回来。除了本类内部，Canvas（导出图片前）
	// 与 History（删除 shape 前）也会调，同 ShapeText::finishEdit
	void finishEdit();
	// ShapeBase 上的统一收尾口子。窗口（关窗 / 缩放 / 导出）与 History（删 shape）都只认它，
	// 没有这个转发的话正在编辑的序号收不了尾：TextBox 不隐藏、编辑标记不清、订阅不摘
	void finishEditing() override { finishEdit(); }
	// 「选择对象」框选要用：圈（带尾时含尾巴尖）加上追加的描述文字那一段。
	// 与 mouseMove 里的命中范围对齐 —— 描述文字本来就是点得中的一块（见 HitDesc）
	bool getShapeBounds(D2D1_RECT_F& out) const override;
	// val 序列化成序号字符串。ToolSub 的样式切换按钮要显示当前样式下的样子，所以是 public static
	static std::wstring serializeVal(const int val, const NumStyle style);
public:
	int val{ 1 };
	bool isEditing{ false };
private:
	// 圆圈外那四个动作按钮（左上 +、左下 −、右上 ×、右下 A）
	enum OpBtn { Plus, Minus, Remove, Text };
	// hoverDraggerIndex 的取值：0~2 是几何控制点（圆心 / 箭头尖 / 半径，见 mouseUp），
	// 之后是那四个动作按钮与描述文本的落点
	enum Hit { HitPlus = 3, HitMinus, HitRemove, HitText, HitDesc };
	// 编号加减：本号走一格、比它大的编号也跟着走一格（见 .cpp 里的顺移说明）
	void bumpVal(int delta);
	D2D1_POINT_2F localPoint(const float degrees);
	D2D1_POINT_2F transformPoint(const D2D1_POINT_2F& point);
	void makePath();
	// 半径变化后要重排文字，两处调用点合到一起
	void makeTextLayout();
	// 外圈带不带指向尾巴。不带尾巴时那个"指向"控制点没有意义，不画也不响应
	bool hasTail() const;
	// 圆圈外那四个动作按钮的位置（它们不跟尾巴转，恒在四个斜角上）
	void updateValueBtns();
	// 画其中一个动作按钮。box 是它的外接方框，kind 决定画 + / − / × / A
	void paintOpBtn(ID2D1DeviceContext* ctx, const D2D1_RECT_F& box, OpBtn kind);
	// 描述文本的引线：从圆周引一段斜线到"转折点"，再从转折点横着铺一条横线，
	// 文字压在横线上（参考 pixpin）。横线恒为横线、只有连圆那一段是斜的，
	// 所以文字永远不会被引线穿过去
	void paintDesc(ID2D1DeviceContext* ctx);
	// 引线转折点（画布像素）：descDx / descDy 记的就是它，拖拽改的也是它
	D2D1_POINT_2F descJoint() const { return D2D1::Point2F(cx + descDx, cy + descDy); }
	// 描述文字的左边缘。落点在圆心右侧就往右排，拖到左侧就整段翻过去
	float descTextX(float textW) const;
	// 横线离圆远的那一端的 x。横线从转折点铺到这儿，两端都留出 descLead 的余量
	float descLineFar(float textW) const;
	// 描述文本左上角的位置（画布像素）。编辑时文本框要落在同一个点上，否则收工会跳一下
	D2D1_POINT_2F descTextPos() const;
	// 描述文本落点的控制点方框。它是算出来的而不是存下来的 —— 拖拽时每帧都在变，
	// 存一份就得在"拖拽 / 改半径 / 切换外圈样式"每一处同步
	D2D1_RECT_F descHandleRect() const;
	// 描述文字的外接方框（画布像素）。点它就进编辑 —— 用户不必非得找到 A 按钮
	D2D1_RECT_F descTextRect() const;
	// 编辑器刚弹出来那一下的命中矩形（画布像素）：整个序号连同描述那块一起框进来。
	// 见 startEdit 里的说明 —— 只留输入框本身的话，开启编辑的那一下点击会被
	// TextBox 当成"点在框外"，刚打开的编辑器当场就被关掉
	D2D1_RECT_F editHitRect() const;
	// 圈 / 方块内部是否命中。以前只有圆心那个小方框算数，圈画得挺大却非得点正中心
	// 才选得中（用户反馈"很难选中历史标号"）
	bool hitInside(const float x, const float y) const;
	// 圆心到转折点的默认距离
	float descGap() const { return r * 0.35f; }
	// 转折点到文字、以及横线末端伸出去的那一段余量
	float descLead() const { return r * 0.45f; }
	// 从 ToolSub 拉一份当前样式
	void setAttr();
	// 圆圈里显示的那个编号字符串
	std::wstring displayText();
	void startEdit();
private:
	std::vector<D2D1_RECT_F> draggers;
	// 那四个动作按钮的外接方框
	D2D1_RECT_F valuePlus{}, valueMinus{}, valueRemove{}, valueText{};
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutText;
	// 追加在圆圈右侧的描述文本。它与编号分开排，所以是第二份 layout
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutDesc;
	// "A" 那个按钮上的字形。按钮半径没变就复用 —— paint 每帧都会走到这儿，
	// 每帧重建一份 TextLayout 纯属浪费
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutBtnText;
	float btnTextSize{ 0.f };
	// 文字编辑期间挂在 Canvas 那个共用 TextBox 上的两个订阅，退出编辑必须摘掉
	winrt::event_token textChangedTok{}, focusTok{};
	// cx/cy 在 mouseDown 里才落定，之间可能先 paint 一次，给初值免得读到垃圾值
	float pressX{ 0.f }, pressY{ 0.f }, cx{ 0.f }, cy{ 0.f }, r{ 0.f }, angle{ 270.f };
	// 描述文本落点（转折点）相对圆心的偏移。默认在圆圈的右下方 —— 引线因此有一段可见的
	// 斜线（正是 pixpin 那个样子：斜线下来、再横着拐出去），之后由用户拖拽改（见 mouseDrag）
	float descDx{ 0.f }, descDy{ 0.f };
	D2D1_POINT_2F tip{ 0,0 }, mid{ 0,0 };
	bool isFill{ false }, isWheel{ false };
	// 追加在圆圈右侧的描述文本。空串表示没有
	std::wstring customText;
	// 当前颜色 RGBA 原值。Ling::Color 没有从 D2D1_COLOR_F 构造的口子，TextBox::setColor 得用它
	UINT32 colorValue{ 0xFF000000 };
	NumStyle numStyle{ NumStyle::Arabic };
	RingStyle ringStyle{ RingStyle::Circle };
};
