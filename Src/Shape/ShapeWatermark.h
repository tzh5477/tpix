#pragma once
#include <include/Ling.h>
#include <ctime>
#include <string>
#include <vector>
#include "ShapeBase.h"

// 文字水印。单击落一个水印层：平铺整张底图，或者按「位置」摆在四角 / 上下中 / 正中。
// 样式（文字 / 字号 / 字体 / 颜色 / 透明度 / 位置 / 旋转）每次画的时候直接从工具条读，
// 所以工具条上改任何一项，图上已画的水印立刻跟着变，不需要 applyStyle 重排
class ShapeWatermark : public ShapeBase
{
public:
	// 水印落点。值与工具条「位置」下拉的顺序、config.json 里 watermark.pos 一一对应，
	// 也是 ToolSub::watermarkPos 的取值（改动顺序会让老配置串味）
	enum class WmPos { Tile = 0, RightBottom, LeftBottom, RightTop, LeftTop, TopCenter, BottomCenter, Center };
	// 水印文字里可用的时间模板，存进文字（与配置）的就是这个串 ——
	// 水印内容弹窗的「时间」下拉显示的则是"按当前时刻展开之后的样子"，比显示模板好认；
	// 选中一档就是把这里的模板串填进输入框。
	// 顺序就是下拉里的顺序，九种最常用的写法
	//（完整日期三种、年月一种、日期加时间两种、纯时间两种、紧凑一种）
	static const std::vector<std::wstring>& timeFormats();
	// 把 text 里的 {yyyy} / {MM} / {dd} / {HH} / {mm} / {ss} 换成 stamp 这一刻的值。
	// 没有占位符就原样返回（也不会去碰花括号里的其它内容）
	static std::wstring expandTime(const std::wstring& text, std::time_t stamp);
	// 「编辑水印内容」弹窗里时间下拉的默认档：{yyyy}年{MM}月{dd}日，就是上面表里的第 4 档。
	// 开一个名字给调用方，而不是让它按下标去取 —— 那张表的顺序是有讲究的（有前缀关系的
	// 档必须长串在前，见上面的说明），按下标取会在别人调整顺序时静默错档
	static const std::wstring& defaultTimeFormat();
	ShapeWatermark(Canvas* win);
	~ShapeWatermark();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	// 单击就是它的正常用法：落一个水印层，不需要拖动
	bool isValidWithoutDrag() override { return true; };
	void setCursor() override;
	std::unique_ptr<ShapeBase> snapshot() const override;
	// 只有水印工具下才吃悬停：别的工具下它不是"图上某一块"，而是整张图的背景层，
	// 一旦参与命中就会把其它工具的每一下点击都截胡（见 .cpp 里的说明）
	void mouseMove(const float x, const float y) override;
private:
	// 按当前样式建文字布局。返回 false 表示没有可画的东西（没写文字）
	bool makeLayout();
	// 在 (x, y) 处以 rotation 角度画一次文字，布局以该点为中心。
	// outer 是外层已有的变换（屏幕上是缩放、导出时是单位阵），必须左乘保住
	void drawOne(ID2D1DeviceContext* ctx, float x, float y, float rotation,
		const D2D1_MATRIX_3X2_F& outer);
	// 非平铺时的落点（底图坐标）：按位置把水印摆在四角 / 上下中 / 正中，
	// 四周留一点边距，免得压着图的边线
	D2D1_POINT_2F anchorPos(WmPos pos, float imgW, float imgH) const;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	// 缓存 makeLayout 的结果，paint 里判断"有没有东西可画"用
	float textW{ 0.f }, textH{ 0.f };
	// 时间占位符是在"文字内容变了"那一刻求值的，之后重画沿用同一个 stamp ——
	// 见 makeLayout 里的说明
	std::wstring lastText;
	std::time_t stamp{ 0 };
};
