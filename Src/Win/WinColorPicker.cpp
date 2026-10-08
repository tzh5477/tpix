#include "pch.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include "../App.h"
#include "../Lang.h"
#include "../Tool/ToolSub.h"
#include "WinColorPicker.h"

namespace {
	class Picker;
	std::unique_ptr<Picker> picker;
	// 声明在类定义之前：Picker 的 onDestroy 里要按地址比对后把它放掉
	void onPickerDestroy(void* p);

	// 列表开着的时候挂一个低级鼠标钩子，用来发现"点到别处去了"。
	// 只有它能在本窗口之外也收得到点击 —— 本窗口自己的 onMouseDown 只看得到自己这一亩地。
	// WH_MOUSE_LL 不需要 DLL，回调回到装它的那条线程（UI 线程，有消息泵）。
	// 与 SelectPopup 同一套做法
	HHOOK mouseHook{ nullptr };
	// 锚点（色板行末尾那块）的屏幕矩形，物理像素。存矩形而不是存指针：
	// 工具条换一次工具就把按钮重建了，指针会野
	RECT anchorRect{};
	// 宿主挪位置时取色器要跟着收，否则它悬在原来的屏幕坐标上
	Ling::WinBase* ownerWin{ nullptr };
	winrt::event_token movedTok{};

	// 本窗口的窗口过程外面套的那一层，只拦 WM_MOUSEACTIVATE 一条。
	// WS_EX_NOACTIVATE 只管得住"程序主动激活"；**鼠标点上来**那一下 DefWindowProc
	// 一律回 MA_ACTIVATE，不认这个扩展样式 —— 于是点一下面板，激活从宿主挪到面板上，
	// 正在编辑的文字会收到 WM_KILLFOCUS 被打断。答 MA_NOACTIVATE，鼠标消息照旧进得来
	WNDPROC prevProc{ nullptr };
	LRESULT CALLBACK pickerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
		return CallWindowProcW(prevProc, hwnd, msg, wp, lp);
	}

	// ---- 版面常量（逻辑像素）----
	constexpr float pad{ 10.f };
	constexpr float svW{ 200.f }, svH{ 132.f };   // SV 方块
	constexpr float hueW{ 14.f };                 // 色相竖条
	constexpr float gap{ 8.f };
	constexpr float rowW{ svW + gap + hueW };     // 上面那一行画布的宽度
	constexpr float infoH{ 26.f };                // 预览块 + 色号
	constexpr float titleH{ 18.f };
	constexpr float cell{ 20.f };                 // 格子的**含外边距**步距
	constexpr int cols{ 10 };
	constexpr float gridW{ cell * cols };
	constexpr float btnH{ 26.f }, btnW{ 66.f };
	// 两排标题之间的小间距
	constexpr float tight{ 4.f };

	// 行数按**实际个数**算，不能写死：预设只有 9 个（cols=10），写死 4 行就有 3 行
	// 是空的 —— 表现是"常用颜色"下面挂着一大片白，看着像没画完。
	// 一块色都没有的那排留 1 行，让标题下面有个落点（完全不留会像是这一块没做）
	int gridRows(int count)
	{
		return count <= 0 ? 1 : (count + cols - 1) / cols;
	}

	constexpr float winW{ pad * 2 + rowW };
	// 必须与 onCreated 里实际摆出来的那几行一一对应，多算会变成底部一截空白
	//（flex 列里每一项都不 grow，多出来的高就留在窗口底部），少算会把按钮挤出去。
	// 逐项：上下 pad + SV 方块 + info 的 marginTop + info + 两个标题各自的 marginTop
	//       + 两块栅格 + 按钮行的 marginTop + 按钮
	float winHeight(int presetCount, int customCount)
	{
		return pad * 2 + svH + gap + infoH
			+ tight + titleH + gridRows(presetCount) * cell
			+ tight + titleH + gridRows(customCount) * cell
			+ gap + btnH;
	}

	struct Rgb { float r{ 0.f }, g{ 0.f }, b{ 0.f }; };

	// 标准 HSV → RGB。h 收在 [0,360)
	Rgb hsvToRgb(float h, float s, float v)
	{
		h = std::fmod(h, 360.f);
		if (h < 0.f) h += 360.f;
		const auto c = v * s;
		const auto x = c * (1.f - std::fabs(std::fmod(h / 60.f, 2.f) - 1.f));
		const auto m = v - c;
		float r{ 0.f }, g{ 0.f }, b{ 0.f };
		if (h < 60.f) { r = c; g = x; }
		else if (h < 120.f) { r = x; g = c; }
		else if (h < 180.f) { g = c; b = x; }
		else if (h < 240.f) { g = x; b = c; }
		else if (h < 300.f) { r = x; b = c; }
		else { r = c; b = x; }
		return Rgb{ r + m, g + m, b + m };
	}

	// 反过来：把工具当前用的颜色拆成 HSV，好让面板一打开就停在那个颜色上
	void rgbToHsv(UINT32 rgba, float& h, float& s, float& v)
	{
		const auto r = ((rgba >> 24) & 0xFF) / 255.f;
		const auto g = ((rgba >> 16) & 0xFF) / 255.f;
		const auto b = ((rgba >> 8) & 0xFF) / 255.f;
		const auto mx = std::max({ r, g, b });
		const auto mn = std::min({ r, g, b });
		const auto d = mx - mn;
		v = mx;
		s = mx <= 0.f ? 0.f : d / mx;
		if (d <= 0.f) { h = 0.f; return; }
		if (mx == r) h = 60.f * std::fmod((g - b) / d, 6.f);
		else if (mx == g) h = 60.f * ((b - r) / d + 2.f);
		else h = 60.f * ((r - g) / d + 4.f);
		if (h < 0.f) h += 360.f;
	}

	// 颜色 = RRGGBBAA，与 Ling::Color(uint32_t) 以及 ToolSub 各处同一个排法
	UINT32 packRgb(const Rgb& c)
	{
		const auto q = [](float v) { return (UINT32)std::lround(std::clamp(v, 0.f, 1.f) * 255.f); };
		return (q(c.r) << 24) | (q(c.g) << 16) | (q(c.b) << 8) | 0xFF;
	}

	// 色号文本。面板上只是只读显示（本窗口不能拿焦点，见头文件里的说明）
	std::wstring hexText(UINT32 rgba)
	{
		return std::format(L"#{:02X}{:02X}{:02X}", (rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF);
	}

	// 色块上的对勾用黑还是白。与 ToolSub.cpp 里那个同一套判据（那边是文件内的，
	// 这里不复用是为了不让两个窗口互相依赖）
	UINT32 checkInkOn(UINT32 rgba)
	{
		const auto r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF;
		return (0.299 * r + 0.587 * g + 0.114 * b) > 150 ? 0x000000FF : 0xFFFFFFFF;
	}

	bool isLightColor(UINT32 rgba)
	{
		const auto r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF;
		return (0.299 * r + 0.587 * g + 0.114 * b) > 200;
	}

	class Picker : public Ling::WinBase
	{
	public:
		explicit Picker(ToolSub* sub) : sub{ sub }
		{
			// 不激活：见头文件里那段说明。TOPMOST 保证它盖在工具条之上
			createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP);
			// 换掉窗口过程：只为了让"鼠标点上来"这一下不激活本窗口（见 pickerProc）
			prevProc = reinterpret_cast<WNDPROC>(
				SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&pickerProc)));
			onDestroy.add([this]() {
				// 不能在销毁回调里同步 reset 自己
				Ling::App::get()->dq.TryEnqueue([this]() { onPickerDestroy(this); });
				});
		}

		// 鼠标的屏幕坐标（物理像素）是否落在本窗口里
		bool isScreenPosIn(POINT pt) const
		{
			return pt.x >= x && pt.x < x + w && pt.y >= y && pt.y < y + h;
		}

	private:
		// 色板表里预设之外的那一段（用户存下来的自定义色）
		int customCount() const
		{
			return std::max(0, (int)sub->colorTable().size() - sub->presetCount());
		}

		// 面板比 Ling 默认的最小跟踪尺寸（800×600）小得多。不放开的话 SetWindowPos
		// 会被系统按回 800×600，面板变成屏幕上好大一块白板
		void onMinMaxInfo(MINMAXINFO* mmi) override
		{
			mmi->ptMinTrackSize.x = 1;
			mmi->ptMinTrackSize.y = 1;
		}

		void onCreated() override
		{
			// 拖 SV 方块 / 色相条靠窗口级的鼠标事件。Canvas 只负责画，收不到鼠标
			//（与 ToolSub 里那条注释同一个原因），所以命中判定得自己按节点坐标做
			onMouseDown.add([this](POINT pos, bool isRight) { onDown(pos, isRight); });
			onMouseMove.add([this](POINT pos) { onMove(pos); });
			onMouseUp.add([this](POINT, bool) { onUp(); });

			// 初值取自工具当前用的颜色 —— 面板一打开就停在那个颜色上，而不是每次都从头
			rgbToHsv(sub->getSelectedColorValue(), hue, sat, val);

			body->setBg(0xFFFFFFFF);
			body->setBorder(1.f, 0xE0E0E0FF);
			body->setBorderRadius(6.f);
			body->setFlexDirection(Ling::FlexDirection::Column);
			body->setPadding(pad, pad, pad, pad);

			// —— 第一行：SV 方块 + 色相竖条 ——
			auto top = body->makeChild<Ling::Node>();
			top->setFlexDirection(Ling::FlexDirection::Row);
			top->setHeight(svH);
			top->setFlexShrink(0.f);
			svCanvas = top->makeChild<Ling::Canvas>();
			svCanvas->setSize(svW, svH);
			// 交换链后端：拖 SV 光标时是每帧重画，单缓冲那张纹理可能被合成器采到
			// "擦干净了、还没画上"的中间态 —— 表现就是拖的时候整个方块闪一下白。
			// 必须在第一次 startPaint 之前调，之后调无效
			svCanvas->enableSwapChain();
			hueCanvas = top->makeChild<Ling::Canvas>();
			hueCanvas->setSize(hueW, svH);
			// 色相条同理（拖它的时候标记在动）
			hueCanvas->enableSwapChain();
			hueCanvas->setMarginLeft(gap);

			// —— 第二行：预览块 + 色号 ——
			auto info = body->makeChild<Ling::Node>();
			info->setFlexDirection(Ling::FlexDirection::Row);
			info->setAlignItems(Ling::Align::Center);
			info->setHeight(infoH);
			info->setMarginTop(gap);
			info->setFlexShrink(0.f);
			preview = info->makeChild<Ling::Label>();
			preview->setSize(24.f, 22.f);
			preview->setBorderRadius(3.f);
			preview->setBorder(1.f, 0xC0C0C0FF);
			hexLabel = info->makeChild<Ling::Label>();
			hexLabel->setFontSize(12.f);
			hexLabel->setColor(0x555555FF);
			hexLabel->setMarginLeft(8.f);
			hexLabel->setFlexGrow(1.f);

			// —— 常用色 ——
			addTitle(Lang::get(L"color.common"));
			presetGrid = addGrid(gridRows(sub->presetCount()));
			// —— 自定义色（用户按「确定」存下来的）——
			addTitle(Lang::get(L"color.custom"));
			customGrid = addGrid(gridRows(customCount()));

			// —— 按钮 ——
			auto btns = body->makeChild<Ling::Node>();
			btns->setFlexDirection(Ling::FlexDirection::Row);
			btns->setJustifyContent(Ling::Justify::End);
			btns->setHeight(btnH);
			btns->setMarginTop(gap);
			btns->setFlexShrink(0.f);
			makeBtn(btns, Lang::get(L"color.cancel"), [this]() { WinColorPicker::close(); }, false, 0);
			makeBtn(btns, Lang::get(L"color.ok"), [this]() {
				// 只在这一下才落盘：拖动 SV 的过程中随时都在变色，一路写盘既没必要
				// 也会把色板塞满一层拖过去的中间色
				sub->applyColor(currentColor());
				WinColorPicker::close();
				}, true, 8.f);

			buildSwatches();
			syncInfo();
		}

		void addTitle(const std::wstring& text)
		{
			auto t = body->makeChild<Ling::Label>();
			t->setText(text);
			t->setFontSize(12.f);
			t->setColor(0x888888FF);
			t->setHeight(titleH);
			// 第一排标题（常用色）上面已经吃了 info 那一段的 gap，这里再补一点点就够
			t->setMarginTop(tight);
			t->setFlexShrink(0.f);
		}

		Ling::Node* addGrid(int rows)
		{
			auto grid = body->makeChild<Ling::Node>();
			grid->setFlexDirection(Ling::FlexDirection::Row);
			grid->setFlexWrap(Ling::Wrap::Wrap);
			// 宽度得是确定的，折行才按它算（yoga 默认 NoWrap，且容器宽度不定时
			// 一格都折不动）
			grid->setWidth(gridW);
			grid->setHeight(rows * cell);
			grid->setFlexShrink(0.f);
			// 栅格比上面那行画布窄（一行放 10 格），居中对齐——
			// 直接左对齐的话右边会空出一小条，看着像少画了一格
			grid->setMarginLeft((rowW - gridW) / 2.f);
			return grid;
		}

		// 一行里的两个按钮外形一致，只有主次之分
		void makeBtn(Ling::Node* parent, const std::wstring& text,
			std::function<void()> onClick, bool primary, float marginLeft)
		{
			auto b = parent->makeChild<Ling::Button>();
			b->setSize(btnW, btnH);
			b->setText(text);
			b->setFontSize(13.f);
			b->setMarginLeft(marginLeft);
			b->setBorderRadius(4.f);
			if (primary) {
				b->setBg(0x1677FFFF);
				b->setHoverBg(0x4090FFFF);
				b->setColor(0xFFFFFFFF);
				b->setHoverColor(0xFFFFFFFF);
			}
			else {
				b->setBg(0xF2F2F2FF);
				b->setHoverBg(0xE6F4FFFF);
				b->setColor(0x333333FF);
				b->setHoverColor(0x1677FFFF);
			}
			b->onClick.add([onClick](Ling::Button*) { onClick(); });
		}

		// 按色板表铺格子：预设在前、自定义在后（表本身就是这个顺序）。
		// 点中任意一格就是直接用它 —— 这些格子本来就对应表里的一项，走 setColorIndex
		void buildSwatches()
		{
			const auto& table = sub->colorTable();
			const auto pc = (size_t)sub->presetCount();
			for (size_t i = 0; i < table.size(); i++) {
				auto* grid = (i < pc) ? presetGrid : customGrid;
				if (!grid) continue;
				// 自定义那几排满了就不摆 —— 多出来的硬塞会把弹窗撑高，底下的按钮被推出可视区
				if (i >= pc && (i - pc) >= (size_t)(cols * gridRows(customCount()))) break;
				addSwatch(grid, table[i], i);
			}
		}

		void addSwatch(Ling::Node* grid, UINT32 color, size_t idx)
		{
			// 色块就是按钮本身（底色 = 颜色，对勾用它自己的 Text）。
			// 不另起一个 Label 装色块：Button 构造里已经 makeChild<Text> 了一个子节点，
			// 再加一个子 Label，两个子节点会在按钮里竖着摞 —— 20 像素的格子装不下
			// 一个 16 高的色块加一行文字，色块会被压扁 / 顶出去。
			// 一个子节点就没有这个问题（宽高都用不完，Text 只在对勾那一下有高度）
			auto btn = grid->makeChild<Ling::Button>();
			btn->setSize(cell - 4.f, cell - 4.f);
			btn->setMargin(2.f);          // 四周各 2 的缝，格子步距仍是 cell
			btn->setBg(color);
			btn->setBorderRadius(3.f);
			btn->setFontFamily(L"icon");
			btn->setFontSize(9.f);
			// 对勾的黑白跟着底色走，浅色块还要描一圈边，否则白块在白色面板上是个看不见的洞
			btn->setColor(checkInkOn(color));
			btn->setHoverColor(checkInkOn(color));
			if (isLightColor(color)) btn->setBorder(1.f, 0xA8A8A8FF);
			// 当前用的那个颜色打勾。只按建面板那一刻算 —— 拖 SV 的过程中不打勾更新，
			// 那是每帧都要重铺一遍格子的事，而拖动时用户看的是预览块
			if (color == sub->getSelectedColorValue()) btn->setText(L"\ue6ad");
			btn->onClick.add([this, idx](Ling::Button*) {
				// 点格子就是直接用它：这些格子本来就对应表里的一项，只需换下标
				sub->setColorIndex(idx);
				WinColorPicker::close();
				});
		}

		// 节点自己那块矩形里的相对坐标（0~1）。Canvas 收不到鼠标，只能这么换算
		static bool hitNode(Ling::Node* n, POINT p, float& u, float& v)
		{
			if (!n || n->w <= 0.f || n->h <= 0.f) return false;
			if (p.x < n->x || p.x >= n->x + n->w || p.y < n->y || p.y >= n->y + n->h) return false;
			u = (p.x - n->x) / n->w;
			v = (p.y - n->y) / n->h;
			return true;
		}

		void onDown(POINT pos, bool isRight)
		{
			// 右键 = 取消，与 ESC 同一档。面板里没有别的右键语义
			if (isRight) { WinColorPicker::close(); return; }
			float u{ 0.f }, v{ 0.f };
			if (hitNode(svCanvas, pos, u, v)) { drag = Drag::Sv; applyDrag(u, v); }
			else if (hitNode(hueCanvas, pos, u, v)) { drag = Drag::Hue; applyDrag(u, v); }
			if (drag == Drag::None) return;
			// 抓住鼠标：往面板外面拖的时候 SV 方块还得跟着走，
			// 松手时也要保证收得到那一下（否则 drag 一直停在 Sv 上）
			SetCapture(hwnd);
		}

		void onMove(POINT pos)
		{
			if (drag == Drag::None) return;
			// 拖动中鼠标跑出面板也不放手：相对坐标夹进 [0,1] 继续算，
			// 于是"拖到边上"就是取到边界值，而不是把选中点冻在离开时的位置
			float u{ 0.f }, v{ 0.f };
			auto* node = drag == Drag::Sv ? svCanvas : hueCanvas;
			u = std::clamp((pos.x - node->x) / node->w, 0.f, 1.f);
			v = std::clamp((pos.y - node->y) / node->h, 0.f, 1.f);
			applyDrag(u, v);
		}

		void onUp()
		{
			if (drag == Drag::None) return;
			drag = Drag::None;
			ReleaseCapture();
		}

		void applyDrag(float u, float v)
		{
			if (drag == Drag::Sv) {
				sat = u;
				val = 1.f - v;      // 上边是"最亮"，往下压暗
			}
			else if (drag == Drag::Hue) {
				hue = u * 360.f;
			}
			syncInfo();
			// 两个画布的光标位置 / 底色都得跟着重画
			refresh();
		}

		void syncInfo()
		{
			const auto c = currentColor();
			if (preview) preview->setBg(c);
			if (hexLabel) hexLabel->setText(hexText(c));
		}

		UINT32 currentColor() const
		{
			// 纯色相要单独算：HSV 里 s=0 时色相没有意义（出来的永远是灰），
			// 于是色相条拖动时预览块纹丝不动。按"在当前 v 下把 s 拉满"给个纯色相，
			// 拖动才看得出色相在变
			return packRgb(hsvToRgb(hue, sat, val));
		}

		void layout() override
		{
			Ling::WinBase::layout();
			drawSv();
			drawHue();
		}

		void drawSv()
		{
			if (!svCanvas) return;
			auto ctx = svCanvas->startPaint();
			if (!ctx) return;
			const auto w = svCanvas->w, h = svCanvas->h;
			const auto pure = hsvToRgb(hue, 1.f, 1.f);

			// 先铺横向渐变：左边白 → 右边纯色相
			D2D1_GRADIENT_STOP gs[2]{
				D2D1_GRADIENT_STOP{ 0.f, D2D1_COLOR_F{ 1.f, 1.f, 1.f, 1.f } },
				D2D1_GRADIENT_STOP{ 1.f, D2D1_COLOR_F{ pure.r, pure.g, pure.b, 1.f } }
			};
			Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> col;
			if (SUCCEEDED(ctx->CreateGradientStopCollection(gs, 2, &col))) {
				Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush;
				if (SUCCEEDED(ctx->CreateLinearGradientBrush(
					D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES{ D2D1_POINT_2F{ 0.f, 0.f }, D2D1_POINT_2F{ w, 0.f } },
					col.Get(), &brush))) {
					ctx->FillRectangle(D2D1_RECT_F{ 0.f, 0.f, w, h }, brush.Get());
				}
			}
			// 再叠一层纵向渐变：上透明 → 下黑。两层叠起来就是标准的 SV 面
			D2D1_GRADIENT_STOP gs2[2]{
				D2D1_GRADIENT_STOP{ 0.f, D2D1_COLOR_F{ 0.f, 0.f, 0.f, 0.f } },
				D2D1_GRADIENT_STOP{ 1.f, D2D1_COLOR_F{ 0.f, 0.f, 0.f, 1.f } }
			};
			Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> col2;
			if (SUCCEEDED(ctx->CreateGradientStopCollection(gs2, 2, &col2))) {
				Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush2;
				if (SUCCEEDED(ctx->CreateLinearGradientBrush(
					D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES{ D2D1_POINT_2F{ 0.f, 0.f }, D2D1_POINT_2F{ 0.f, h } },
					col2.Get(), &brush2))) {
					ctx->FillRectangle(D2D1_RECT_F{ 0.f, 0.f, w, h }, brush2.Get());
				}
			}

			// 光标：黑白两个圈套着画，深色底和浅色底上都看得见
			const auto cx = sat * w, cy = (1.f - val) * h;
			Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> sb;
			if (SUCCEEDED(ctx->CreateSolidColorBrush(D2D1_COLOR_F{ 1.f, 1.f, 1.f, 1.f }, &sb))) {
				ctx->DrawEllipse(D2D1_ELLIPSE{ D2D1_POINT_2F{ cx, cy }, 6.f, 6.f }, sb.Get(), 1.5f);
				sb->SetColor(D2D1_COLOR_F{ 0.f, 0.f, 0.f, 1.f });
				ctx->DrawEllipse(D2D1_ELLIPSE{ D2D1_POINT_2F{ cx, cy }, 7.5f, 7.5f }, sb.Get(), 1.f);
			}
			svCanvas->finishPaint();
		}

		void drawHue()
		{
			if (!hueCanvas) return;
			auto ctx = hueCanvas->startPaint();
			if (!ctx) return;
			const auto w = hueCanvas->w, h = hueCanvas->h;
			// 七档：红黄绿青蓝品红再回到红，正好一圈
			D2D1_GRADIENT_STOP gs[7];
			for (int i = 0; i <= 6; i++) {
				const auto c = hsvToRgb(i * 60.f, 1.f, 1.f);
				gs[i] = D2D1_GRADIENT_STOP{ i / 6.f, D2D1_COLOR_F{ c.r, c.g, c.b, 1.f } };
			}
			Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> col;
			if (SUCCEEDED(ctx->CreateGradientStopCollection(gs, 7, &col))) {
				Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush;
				if (SUCCEEDED(ctx->CreateLinearGradientBrush(
					D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES{ D2D1_POINT_2F{ 0.f, 0.f }, D2D1_POINT_2F{ 0.f, h } },
					col.Get(), &brush))) {
					ctx->FillRectangle(D2D1_RECT_F{ 0.f, 0.f, w, h }, brush.Get());
				}
			}
			// 当前色相：两条横线夹一个亮块，比单个小三角好认
			const auto cy = (hue / 360.f) * h;
			Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> sb;
			if (SUCCEEDED(ctx->CreateSolidColorBrush(D2D1_COLOR_F{ 0.f, 0.f, 0.f, 1.f }, &sb))) {
				ctx->DrawLine(D2D1_POINT_2F{ 0.f, cy - 3.f }, D2D1_POINT_2F{ w, cy - 3.f }, sb.Get(), 1.f);
				ctx->DrawLine(D2D1_POINT_2F{ 0.f, cy + 3.f }, D2D1_POINT_2F{ w, cy + 3.f }, sb.Get(), 1.f);
				sb->SetColor(D2D1_COLOR_F{ 1.f, 1.f, 1.f, 1.f });
				ctx->DrawLine(D2D1_POINT_2F{ 0.f, cy }, D2D1_POINT_2F{ w, cy }, sb.Get(), 2.f);
			}
			hueCanvas->finishPaint();
		}

	private:
		enum class Drag { None, Sv, Hue };
		ToolSub* sub{ nullptr };
		Ling::Canvas* svCanvas{ nullptr };
		Ling::Canvas* hueCanvas{ nullptr };
		Ling::Label* preview{ nullptr };
		Ling::Label* hexLabel{ nullptr };
		Ling::Node* presetGrid{ nullptr };
		Ling::Node* customGrid{ nullptr };
		Drag drag{ Drag::None };
		// 面板的草稿色（HSV）。松手 / 点「确定」之前不落盘，也不动工具条上的当前色 ——
		// 拖动过程中每改一次都落到工具上，色板会被一路拖过去的中间色填满
		float hue{ 0.f }, sat{ 1.f }, val{ 1.f };
	};

	void onPickerDestroy(void* p)
	{
		if (picker.get() == p) picker.reset();
	}

	LRESULT CALLBACK hookProc(int code, WPARAM wp, LPARAM lp)
	{
		// 每次都要往下传，否则会掐掉别人的鼠标消息
		if (code >= 0 && picker && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN)) {
			// 正在拖 SV / 色相条：那期间窗口拿着 capture，鼠标拖到面板外面也算在拖，
			// 这时候按"点到外面了"收面板等于拖到一半面板没了
			if (GetCapture() == picker->hwnd) return CallNextHookEx(mouseHook, code, wp, lp);
			auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
			POINT pt{ info->pt.x, info->pt.y };
			if (!PtInRect(&anchorRect, pt) && !picker->isScreenPosIn(pt)) WinColorPicker::close();
		}
		return CallNextHookEx(mouseHook, code, wp, lp);
	}
}

void WinColorPicker::show(ToolSub* sub, Ling::Node* anchor)
{
	if (!sub || !anchor) return;
	auto* owner = static_cast<Ling::WinBase*>(sub);
	close();

	auto ox = (float)owner->x;
	auto oy = (float)owner->y;
	auto dpi = owner->dpi > 0.f ? owner->dpi : 1.f;
	const auto pw = (int)std::lround(winW * dpi);
	// 高度随格子数走（预设 9 个只有 1 行），与 onCreated 里摆出来的行数用同一个算法
	const auto customN = std::max(0, (int)sub->colorTable().size() - sub->presetCount());
	const auto ph = (int)std::lround(winHeight(sub->presetCount(), customN) * dpi);
	// 默认弹在按钮上方：工具条一般贴着标注区下沿，往下弹会压住正在画的地方；
	// 上方放不下才翻到下面。用按钮所在显示器的工作区判断，而不是整个虚拟桌面
	POINT at{ (int)(ox + anchor->x + anchor->w / 2.f), (int)(oy + anchor->y) };
	MONITORINFO mi{ sizeof(mi) };
	GetMonitorInfo(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
	int px = at.x - pw / 2;
	int py = at.y - ph - 2;
	if (py < mi.rcWork.top) py = (int)(oy + anchor->y + anchor->h) + 2;
	// 上下都放不下就贴着工作区顶边 —— 面板才 300 出头高，正常屏幕都不会走到这一步
	if (py + ph > mi.rcWork.bottom) py = std::max(mi.rcWork.top, mi.rcWork.bottom - ph);
	if (py < mi.rcWork.top) py = mi.rcWork.top;
	if (px + pw > mi.rcWork.right) px = mi.rcWork.right - pw;
	if (px < mi.rcWork.left) px = mi.rcWork.left;

	anchorRect = RECT{ (int)(ox + anchor->x), (int)(oy + anchor->y),
		(int)(ox + anchor->x + anchor->w), (int)(oy + anchor->y + anchor->h) };
	picker = std::make_unique<Picker>(sub);
	// 这里刻意不走 setSize / setPosition / show()：Ling 的那三个用的是不带 SWP_NOACTIVATE
	// 的 SetWindowPos / ShowWindow，会把这个带 WS_EX_NOACTIVATE 的窗口真正激活 ——
	// 编辑文字时点开取色器就会把焦点从 TextBox 上抢走，正在输入的那行字当场断掉。
	// 尺寸 / 位置 / 置顶 / 显示合并成一次调用，全程带 SWP_NOACTIVATE，不碰前台。
	// x/y/w/h 仍要写回成员：Ling 的命中判定与布局读的是它们
	// x/y 在 WinBase 里是 int（与 Node 的 float x/y 同名遮蔽），w/h 是 float —— 别混写
	picker->x = px;
	picker->y = py;
	picker->w = (float)pw;   // 物理像素，与 WinBase::setSize / setPosition 的口径一致
	picker->h = (float)ph;
	SetWindowPos(picker->hwnd, HWND_TOPMOST, px, py, pw, ph,
		SWP_NOACTIVATE | SWP_SHOWWINDOW);
	ownerWin = owner;
	movedTok = owner->onMoved.add([]() { WinColorPicker::close(); });
	mouseHook = SetWindowsHookEx(WH_MOUSE_LL, hookProc, nullptr, 0);
}

void WinColorPicker::close()
{
	if (mouseHook) {
		UnhookWindowsHookEx(mouseHook);
		mouseHook = nullptr;
	}
	if (ownerWin) {
		ownerWin->onMoved.remove(movedTok);
		ownerWin = nullptr;
	}
	if (!picker) return;
	// 只销毁窗口句柄，C++ 对象推迟到下一轮消息循环 —— 收起多半是从某次点击的栈上发起的
	picker->close();
}

bool WinColorPicker::isOpen()
{
	return picker != nullptr;
}
