#pragma once
#include <memory>
#include <include/Ling.h>
class Canvas;
class ShapeBase
{
public:
	ShapeBase(Canvas* win);
	virtual ~ShapeBase();
	virtual void paint(ID2D1DeviceContext* ctx) = 0;
	virtual void paintDragger(ID2D1DeviceContext* ctx) {};
	virtual void mouseMove(const float x, const float y) { };
	virtual void mouseDrag(const float x, const float y) {};
	virtual void mouseDown(const float x, const float y) {};
	virtual void mouseUp(const float x, const float y) {};
	virtual void mouseWheel(const float x, const float y, const short delta) {};
	virtual void setCursor() {};
	// 选中态下的按键。目前只有序号用它（+/- 改编号、F2 编辑追加的描述文本）
	virtual void onKey(UINT key) {};
	// 编辑态收尾。Canvas 导出图片前、History 删掉 shape 前都会调，
	// 只有会进编辑态的元素（ShapeText / ShapeNumber）实现，其余留空
	virtual void finishEditing() {};
	// ToolSub 上的颜色 / 字号 / 序号样式变了，重新从工具条取样式并重建自己的画刷与几何
	virtual void applyStyle() {};
	// 把工具条上"选中的那一档"（箭头样式 / 线条类型 / 端点 / 线型）套到这一笔上。
	// 与 applyStyle 分开，是因为两者该被触发的时机不同（见 WinPin::onToolStyleChanged）：
	// applyStyle 管"外观"（颜色 / 粗细 / 填充 / 半透明），改哪一样、哪怕只是滚滚轮调粗细，
	// 都该同步到选中的那一笔；而"这一笔长什么形状"只有用户真去动那个下拉时才该跟着变。
	// 合在一起会出这种事：画好箭头 A → 把下拉切到 B → 回头选中 A 滚一下滚轮调粗细，
	// 滚轮那条路走的是 applyStyle，顺手把 A 的档位也改成了 B
	// （作者报的"滚动 / 点填充之后，之前选中的箭头样式会变化"）
	// 有档位可言的元素（箭头 / 线条）覆写它，其余留空
	virtual void applyToolStyle() {};
	// 新建这一笔如果只是按下马上弹起（没有拖动），默认当成什么也没画，元素直接丢掉。
	// 单击本身就是正常用法的元素（number 落徽章、text 进编辑）覆盖它返回 true
	virtual bool isValidWithoutDrag() { return false; };
	// 命中判定要能在 const 上下文里用（动作图标的 hitActionBtn 就是 const 的）
	bool isInRect(const D2D1_RECT_F rect, const float x, const float y) const;
	// 元素在底图坐标系里的外接矩形。用来摆右上角那排动作图标 ——
	// 默认返回 false（水印铺满整图、折线族那几笔用户说不用加），派生类按需覆写。
	// 带旋转的元素要把旋转也算进去（摆图标的是外接框，不是未旋转的那个 rect）
	virtual bool getShapeBounds(D2D1_RECT_F& out) const { return false; }
	// 选中元素外侧那几枚按钮。现在只剩复制与 × 两枚，并成选中框下方的一条迷你条
	//（见 actionBarRect）—— 原来是左上复制、右上 ×、右下旋转手柄各占一个角，与八向手柄
	// 挤在同一圈窄带里；分成四个角时相邻两枚挨得太近，鼠标移过去点错一个就是误删。
	// 派生类自己的动作图标（矩形/圆互转那枚）已经撤掉，换形状统一走工具条
	virtual int actionCount() const { return 0; }
	// 多选（Ctrl 框选）那一圈提示框线 / 标记点，要离 getShapeBounds 给的框再让出多少
	//（底图像素）。默认 0 = 紧贴着画。
	// 为什么需要它：**空心的矩形**画的就是 getShapeBounds 那个矩形本身，描边以它为中线，
	// 于是多选那圈蓝线正好压在红描边的中线上，混成一片紫；框上那 8 个白点又正好落在
	// 四个角与四条边的中点上 —— 作者报的"ctrl 框选之后矩形边框线的颜色会变"。
	// 让出"半个描边宽"就落到描边外沿之外了。
	// 别的族不需要：椭圆族的框是外接矩形（只有 4 个切点碰到弧线）、线条族与箭头的
	// getShapeBounds 本来就含了半个线宽（见 ShapeLineBase::boundsPad）。
	// 注意只影响"多选提示"这一处 —— 动作图标、旋转手柄、框选命中都还是用原框
	virtual float selectionGap() const { return 0.f; }
	// 画第 i 枚动作图标（i 只会在 actionCount 范围内被调到）。c 是图标中心、rad 是原来那个
	// 圆底的半径（图标尺寸按它算）。笔与笔宽由调用方给，不写死在派生类里 ——
	// 没有圆底之后同一份几何要"先白描边、再上原色"画两遍（见 paintIconHaloed）
	virtual void paintActionIcon(ID2D1DeviceContext* ctx, const int i, const D2D1_POINT_2F& c,
		const float rad, ID2D1Brush* brush, const float strokeW) {}
	// 第 i 枚动作图标被点了
	virtual void onAction(const int i) {}
	// 这一样元素能不能复制（见 clone）。默认不行 —— 左上角那枚复制按钮只在覆写成真
	// 的元素上出现。擦除与水印不参与："再盖一块一模一样的"对它们没有意义
	virtual bool copyable() const { return false; }
	// 复制一份自己：几何、样式、画刷颜色都照搬，整体往 (dx, dy) 挪开（底图像素）。
	// 复制出来的这一份还没有归属，由调用方（History::addShape）收下。
	// 派生类写一行 `return cloneSelf(*this, dx, dy, target);` 就够 —— 那套骨架见 cloneSelf。
	// target 非空表示"粘到另一个画布上"：先把这一份的宿主换成它，再跑善后 ——
	// 马赛克那几支的善后（以及 translate 里的重算）要按宿主回读画面，
	// 宿主还指着已经关掉的那个窗口就是访问违例
	virtual std::unique_ptr<ShapeBase> clone(const float dx, const float dy, Canvas* target = nullptr) const { return nullptr; }
	// 批量移动：整块挪 (dx, dy)。translate 是 protected，而 WinPin 的"批量拖动多选那一批"
	// 是从外面逐个调的，得有一个口子
	void moveBy(const float dx, const float dy) { translate(dx, dy); }
	// 批量旋转：绕世界坐标 center 刚体转 deg 度（顺时针为正）。有"角度"这个概念的元素覆写
	//（矩形族 / 文本）；线条、箭头、序号没有这一项，留空 —— 批量旋转直接跳过它们
	virtual void rotateBy(const float deg, const D2D1_POINT_2F& center) {}
	// 整排图标的枚数（含末尾那枚 × 与左上角那枚复制）
	int actionBtnTotal() const { return actionCount() + (copyable() ? 2 : 1); }
	// 第 i 枚图标的方框（底图坐标）。末尾那枚是右上角的 ×，actionCount() 那枚是左上角的
	// 复制（不可复制时它正好等于末尾那枚，两条分支都先认 ×），其余派生类的动作图标在左下角。
	// 没有外接矩形、或 i 越界时返回空框
	D2D1_RECT_F actionBtnRect(const int i) const;
	// 命中的是第几枚图标，没命中返回 -1
	int hitActionBtn(const float x, const float y) const;
	// 装这几枚图标的那条迷你条的方框（底图坐标）。挂在选中框下方居中，翻到顶上时也由它定，
	// actionBtnRect 按它往里分格 —— 两处必须是同一份几何，条与图标才对得齐
	D2D1_RECT_F actionBarRect() const;
	// 画整排图标：只有图标本身，不再垫白色圆底
	void paintActionBtns(ID2D1DeviceContext* ctx);
	// 动作图标的统一画法：先把同一份几何用白笔加粗描一遍当衬底，再用原色笔画上去。
	// 去掉圆底之后浅蓝图标压在浅色底图上会读不出来，垫一圈白边就任何底图都看得清。
	// 用模板而不是 std::function：每帧都要画几枚，省掉一层类型擦除开销
	template <class F>
	void paintIconHaloed(ID2D1DeviceContext* ctx, const float strokeW, const F& paint)
	{
		// halo 用"图标自己的尺度"算，不跟 dpi 走（这个模板在头文件里，Canvas 只有前置声明，
		// 调不到 win->getDpi）。0.27 × draggerSize ≈ 每个图标都有一圈很细的白边；
		// 再宽就糊成一团白，反倒又成了原来的"白圆底"
		paint(brushDraggerFill.Get(), strokeW + draggerSize * 0.27f);
		paint(brushDragger.Get(), strokeW);
	}
	// 命中之后分发：末尾那枚 = 删掉自己，actionCount() 那枚 = 复制一份，其余交给 onAction
	void onActionBtn(const int i);
protected:
	// 复制的公共骨架：拷一份（走隐式拷贝构造 —— 没经过派生类的构造函数，所以"领号"
	// "读工具条当前样式"那些副作用一概不会发生）→ 摘掉"正在拖 / 已撤销"这两个运行态 →
	// fixupCopy() 补各自的善后 → 整块几何挪开。
	// 画刷绝不能跟着拷贝共享：ComPtr 拷过来是同一支画刷，改一方的颜色会连另一方一起改。
	// static 是因为调用它的 clone() 是 const 的，非静态成员函数收不下一个 const this
	template <class T>
	static std::unique_ptr<ShapeBase> cloneSelf(const T& src, const float dx, const float dy, Canvas* target = nullptr)
	{
		auto c = std::make_unique<T>(src);
		// 经 ShapeBase& 去调那两个虚函数：命名类才是 ShapeBase，protected 才访问得到。
		// 直接 c->fixupCopy() 的话名字查到的是派生类里那份重声明（命名类变成派生类），
		// 而从基类的成员里访问派生类的 protected 成员是不许的
		ShapeBase& base = *c;
		// 换宿主必须排在 fixupCopy / translate 之前：那两个都要按画布算（见 clone 的说明）
		if (target) base.win = target;
		base.isUndo = false;
		base.hoverDraggerIndex = -1;
		base.fixupCopy();
		base.translate(dx, dy);
		return c;
	}
	// 复制之后的善后，默认什么都不做。派生类按需补三样：
	//   ① 重建自己那几支画刷（理由见 cloneSelf）
	//   ② 把"指向我自己"的成员改指新的一份（马赛克的 mosaicPaint 就存着这个）
	//   ③ 清掉编辑态（标号 / 文本那两个共用 TextBox 的订阅不能照抄）
	virtual void fixupCopy() {}
	// 整块几何挪 (dx, dy)，并重算由它派生的手柄 / 路径 / 文字排版
	virtual void translate(const float dx, const float dy) {}
public:
	// 所属画布。窗口尺寸、DPI、底图、工具条样式、刷新全从这里出 ——
	// shape 不认识窗口，换一个画布宿主这层照旧能挂上去
	Canvas* win;
	bool isUndo{ false };
	int hoverDraggerIndex{ -1 };
	// 建这一笔时的工具 id，由 History::createShape 填。
	// 「应用到全部」拿它筛同类 —— ToolSub 的颜色是每个工具各存一份的，
	// 跨类型套样式会让文字、序号被矩形的那个颜色污染
	std::wstring toolId;
protected:
	// 旋转手柄的方框（底图坐标）。摆在 getShapeBounds() 给的外接框右下角外侧，
	// 与右上角的 × 同一段距离。手柄跟着外接框走、不跟着图形转 —— 三个角各一枚按钮才对称。
	// 文本与矩形族共用这一份，画法与求角方式完全一样
	void updateRotateDragger();
	// 画旋转手柄。坐标已经是转好之后的，调用方不用再叠旋转
	void paintRotateHandle(ID2D1DeviceContext* ctx);
	// 在指定位置画一枚旋转图标。矩形族把它挂在"角手柄外侧那一圈"上（抓哪个角画在哪个角外），
	// 不再有固定的右下角位置；文本那一族仍走上面那个用 rotateDragger 的版本
	void paintRotateHandleAt(ID2D1DeviceContext* ctx, const D2D1_POINT_2F& c);
	// 鼠标落在 (x,y) 时，相对手柄的静止方向转过了多少度（顺时针为正）。
	// center 是元素中心，rotateRestAngle 由 updateRotateDragger 记下
	float rotateAngleAt(const D2D1_POINT_2F& center, const float x, const float y) const;
	// 绕 c 转 deg 度（与 D2D 的 Rotation 同一套约定：正角度在屏幕上顺时针）
	static D2D1_POINT_2F rotatePoint(const D2D1_POINT_2F& p, const D2D1_POINT_2F& c, const float deg);
	// 同上的反向：命中判定时把鼠标点转回元素自己的坐标系
	static D2D1_POINT_2F unrotatePoint(const D2D1_POINT_2F& p, const D2D1_POINT_2F& c, const float deg);
	// 按画布当前的变换把点映射过去。元素存的是底图坐标，而屏幕上还压着 Canvas 的缩放
	// （Ctrl+滚轮），旋转中心得跟着落到同一层坐标上，否则一缩放就转偏
	static D2D1_POINT_2F transformPoint(ID2D1DeviceContext* ctx, const D2D1_POINT_2F& p);
	// 轴对齐矩形绕自己中心转 deg 度之后的外接矩形。带旋转的元素给动作图标定位要用
	// "看得见的那一块"的外面，不是未旋转的那个 rect
	static D2D1_RECT_F rotatedBounds(const D2D1_RECT_F& r, const float deg);
protected:
	float draggerSize;
	// 控制点的浅蓝描边 + 选中时的白色填充（样式参考 pixpin：浅蓝空心框压在标注线上，
	// 不填白的话线从框中间穿过去，一排点看着全是花的）
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushDragger;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushDraggerFill;
	// 迷你条的配角：淡灰边框、极淡投影、删除那格的红（构造函数里建）
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBarBorder;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBarShadow;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushDelete;
	// 旋转手柄的方框（底图坐标），与它静止时所在的方向（度，顺时针为正、0 = 正上方）
	D2D1_RECT_F rotateDragger{};
	float rotateRestAngle{ 0.f };
	// 手柄的两段几何：圆弧（描边）与两端的箭头（填充）。每帧重建，用 Release 拿地址
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> rotateArc, rotateArrows;
private:
};
