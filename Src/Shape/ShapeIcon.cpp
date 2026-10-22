#include "pch.h"
#include <algorithm>
#include "Canvas.h"
#include "Setting.h"
#include "Tool/ToolSub.h"
#include "History.h"
#include "ShapeIcon.h"

using Microsoft::WRL::ComPtr;

namespace {
	// —— 调色板。这批图标的颜色是"图标语义"的一部分（错误就是红的），不跟工具条色板走 ——
	constexpr UINT32 inkC{ 0x333333 };     // 指针类图标的墨色（与工具条按钮字色同源）
	constexpr UINT32 blueC{ 0x1677FF };    // 信息 / 问题 / 忙碌
	constexpr UINT32 redC{ 0xCF1322 };     // 错误 / 取消
	constexpr UINT32 greenC{ 0x389E0D };   // 确定
	constexpr UINT32 yellowC{ 0xFADB14 };  // 提示的灯泡 / 笑脸的脸
	constexpr UINT32 amberC{ 0xD48806 };   // 灯泡的底座与光线（描边）

	// 圆头描边样式：箭头 / 灯泡光线这类短笔画，圆头比方头柔和得多。
	// StrokeStyle 绑在 d2dFactory 上（不是 device 上），factory 与进程同寿，缓存成 static 安全
	ComPtr<ID2D1StrokeStyle> roundStroke()
	{
		static ComPtr<ID2D1StrokeStyle> s;
		if (!s) {
			Ling::D2D::get()->d2dFactory->CreateStrokeStyle(
				D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
					D2D1_LINE_JOIN_ROUND, 0.f, D2D1_DASH_STYLE_SOLID, 0.f),
				nullptr, 0, s.GetAddressOf());
		}
		return s;
	}

	// 画图标的环境。坐标一律用归一化的 (0~1)，由 p() 映射到方框里 —— 19 个 painter
	// 就能与大图标小图标（工具条 13px / 图上几十 px）共用同一套几何。
	// alpha 是整体不透明度（用户调的"透明度"），mono 是"影子那一次"：所有颜色强制换成
	// 半透明黑 —— 同一份 painter 跑两遍（一遍影子、一遍正身），不用为影子单独维护一套几何
	struct IconKit {
		ID2D1DeviceContext* ctx;
		ID2D1SolidColorBrush* brush;   // 共用一支，每个图元画之前现设色
		D2D1_RECT_F box;               // 画在哪个方框里（物理像素）
		float alpha{ 1.f };
		bool mono{ false };            // true = 影子那次：全画成半透明黑
		float ox{ 0.f }, oy{ 0.f };    // 整体偏移（影子往右下错两像素用的）
		float dpi{ 1.f };
		float W() const { return box.right - box.left; }
		float H() const { return box.bottom - box.top; }
		D2D1_POINT_2F p(const float nx, const float ny) const
		{
			return { box.left + nx * W() + ox, box.top + ny * H() + oy };
		}
		D2D1_COLOR_F color(const UINT32 rgb) const
		{
			if (mono) return D2D1::ColorF(0x000000, 0.25f * alpha);
			return D2D1::ColorF(rgb, alpha);
		}
		// 白衬底的宽度。指针类图标是深墨色，压在深色底图上会读不出 —— 垫一圈白边
		//（同 ShapeBase::paintIconHaloed 的道理）。影子那一次不垫（影子就是一片黑影）
		float halo(const float w) const { return mono ? 0.f : w + 1.5f * dpi; }

		// 一段闭合多边形（可含多个 figure，全部一起做白衬底 + 填色，避免多块拼接处
		// 的衬底互相切开）。pts 里连续两点之间连线，每个 figure 闭合
		void fillFigs(const std::vector<std::vector<D2D1_POINT_2F>>& figs, const UINT32 rgb, const float haloW = 0.f)
		{
			ComPtr<ID2D1PathGeometry> geo;
			if (FAILED(Ling::D2D::get()->d2dFactory->CreatePathGeometry(geo.GetAddressOf()))) return;
			ComPtr<ID2D1GeometrySink> sink;
			if (FAILED(geo->Open(sink.GetAddressOf()))) return;
			for (auto& fig : figs) {
				if (fig.size() < 2) continue;
				sink->BeginFigure(fig[0], D2D1_FIGURE_BEGIN_FILLED);
				for (size_t i = 1; i < fig.size(); i++) sink->AddLine(fig[i]);
				sink->EndFigure(D2D1_FIGURE_END_CLOSED);
			}
			sink->Close();
			if (haloW > 0.f) {
				brush->SetColor(D2D1::ColorF(0xFFFFFF, alpha));
				ctx->DrawGeometry(geo.Get(), brush, haloW, roundStroke().Get());
			}
			brush->SetColor(color(rgb));
			ctx->FillGeometry(geo.Get(), brush);
		}
		// 一组折线（figure 不闭合）。round = 圆头
		void strokeFigs(const std::vector<std::vector<D2D1_POINT_2F>>& figs, const UINT32 rgb,
			const float w, const float haloW = 0.f)
		{
			ComPtr<ID2D1PathGeometry> geo;
			if (FAILED(Ling::D2D::get()->d2dFactory->CreatePathGeometry(geo.GetAddressOf()))) return;
			ComPtr<ID2D1GeometrySink> sink;
			if (FAILED(geo->Open(sink.GetAddressOf()))) return;
			for (auto& fig : figs) {
				if (fig.size() < 2) continue;
				sink->BeginFigure(fig[0], D2D1_FIGURE_BEGIN_HOLLOW);
				for (size_t i = 1; i < fig.size(); i++) sink->AddLine(fig[i]);
				sink->EndFigure(D2D1_FIGURE_END_OPEN);
			}
			sink->Close();
			if (haloW > 0.f) {
				brush->SetColor(D2D1::ColorF(0xFFFFFF, alpha));
				ctx->DrawGeometry(geo.Get(), brush, haloW, roundStroke().Get());
			}
			brush->SetColor(color(rgb));
			ctx->DrawGeometry(geo.Get(), brush, w, roundStroke().Get());
		}
		// 圆弧描边（忙碌的 Spinner / 笑脸的嘴）。角度是屏幕惯例：0 度朝右、90 度朝下（y 向下），
		// 顺时针扫过去；|a1-a0| > 180 时必须给 LARGE，否则 D2D 挑小弧画出来缺一截
		void arc(const float nx, const float ny, const float r, float a0, float a1,
			const UINT32 rgb, const float w, const float haloW = 0.f)
		{
			auto pt = [&](float deg) {
				auto rad = deg * 3.14159265358979323846f / 180.f;
				auto c = p(nx, ny);
				return D2D1::Point2F(c.x + r * W() * cosf(rad), c.y + r * H() * sinf(rad));
			};
			ComPtr<ID2D1PathGeometry> geo;
			if (FAILED(Ling::D2D::get()->d2dFactory->CreatePathGeometry(geo.GetAddressOf()))) return;
			ComPtr<ID2D1GeometrySink> sink;
			if (FAILED(geo->Open(sink.GetAddressOf()))) return;
			sink->BeginFigure(pt(a0), D2D1_FIGURE_BEGIN_HOLLOW);
			sink->AddArc(D2D1::ArcSegment(pt(a1), D2D1::SizeF(r * W(), r * H()), 0.f,
				D2D1_SWEEP_DIRECTION_CLOCKWISE,
				fabsf(a1 - a0) > 180.f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
			sink->EndFigure(D2D1_FIGURE_END_OPEN);
			sink->Close();
			if (haloW > 0.f) {
				brush->SetColor(D2D1::ColorF(0xFFFFFF, alpha));
				ctx->DrawGeometry(geo.Get(), brush, haloW, roundStroke().Get());
			}
			brush->SetColor(color(rgb));
			ctx->DrawGeometry(geo.Get(), brush, w, roundStroke().Get());
		}
		void disc(const float nx, const float ny, const float r, const UINT32 rgb, const float ringW = 0.f)
		{
			auto c = p(nx, ny);
			auto e = D2D1::Ellipse(c, r * W(), r * H());
			if (ringW > 0.f) {   // 空心圆：先白衬底再描边（取消那枚红圈用）
				if (auto hw = halo(ringW)) {
					brush->SetColor(D2D1::ColorF(0xFFFFFF, alpha));
					ctx->DrawEllipse(e, brush, hw, roundStroke().Get());
				}
				brush->SetColor(color(rgb));
				ctx->DrawEllipse(e, brush, ringW, roundStroke().Get());
				return;
			}
			if (auto hw = halo(ringW)) {
				brush->SetColor(D2D1::ColorF(0xFFFFFF, alpha));
				ctx->DrawEllipse(e, brush, hw, roundStroke().Get());
			}
			brush->SetColor(color(rgb));
			ctx->FillEllipse(e, brush);
		}
		// 圆角矩形（实心）。鼠标轮廓那枚描边版走 disc 同款的 ring 语义不合适，单独一个
		void rrectFill(const float x0, const float y0, const float x1, const float y1,
			const float rad, const UINT32 rgb)
		{
			ComPtr<ID2D1PathGeometry> geo;
			if (FAILED(Ling::D2D::get()->d2dFactory->CreatePathGeometry(geo.GetAddressOf()))) return;
			ComPtr<ID2D1GeometrySink> sink;
			if (FAILED(geo->Open(sink.GetAddressOf()))) return;
			addRRect(sink.Get(), x0, y0, x1, y1, rad);
			sink->Close();
			brush->SetColor(color(rgb));
			ctx->FillGeometry(geo.Get(), brush);
		}
		void rrectStroke(const float x0, const float y0, const float x1, const float y1,
			const float rad, const UINT32 rgb, const float w, const float haloW = 0.f)
		{
			ComPtr<ID2D1PathGeometry> geo;
			if (FAILED(Ling::D2D::get()->d2dFactory->CreatePathGeometry(geo.GetAddressOf()))) return;
			ComPtr<ID2D1GeometrySink> sink;
			if (FAILED(geo->Open(sink.GetAddressOf()))) return;
			addRRect(sink.Get(), x0, y0, x1, y1, rad);
			sink->Close();
			if (haloW > 0.f) {
				brush->SetColor(D2D1::ColorF(0xFFFFFF, alpha));
				ctx->DrawGeometry(geo.Get(), brush, haloW, roundStroke().Get());
			}
			brush->SetColor(color(rgb));
			ctx->DrawGeometry(geo.Get(), brush, w, roundStroke().Get());
		}
		// 往 sink 里补一个圆角矩形的 figure（四角各一段 90 度弧）。调用方负责 Open / Close
		void addRRect(ID2D1GeometrySink* sink, float x0, float y0, float x1, float y1, float rad)
		{
			x0 = box.left + x0 * W() + ox; y0 = box.top + y0 * H() + oy;
			x1 = box.left + x1 * W() + ox; y1 = box.top + y1 * H() + oy;
			rad *= W();
			rad = std::min(rad, std::min(x1 - x0, y1 - y0) / 2.f);
			sink->BeginFigure(D2D1::Point2F(x0 + rad, y0), D2D1_FIGURE_BEGIN_FILLED);
			sink->AddLine(D2D1::Point2F(x1 - rad, y0));
			sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x1, y0 + rad), D2D1::SizeF(rad, rad), 0.f,
				D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
			sink->AddLine(D2D1::Point2F(x1, y1 - rad));
			sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x1 - rad, y1), D2D1::SizeF(rad, rad), 0.f,
				D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
			sink->AddLine(D2D1::Point2F(x0 + rad, y1));
			sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x0, y1 - rad), D2D1::SizeF(rad, rad), 0.f,
				D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
			sink->AddLine(D2D1::Point2F(x0, y0 + rad));
			sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x0 + rad, y0), D2D1::SizeF(rad, rad), 0.f,
				D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
			sink->EndFigure(D2D1_FIGURE_END_CLOSED);
		}
		// 一段文字（问号的 "?"）。用工具条同款的基础文本格式，字号按方框边长的比例给
		void text(const wchar_t* s, const UINT32 rgb, const float sizeRatio)
		{
			auto layout = Ling::D2D::makeTextLayout(s, sizeRatio * H(), W(), H());
			if (!layout) return;
			layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
			layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
			brush->SetColor(color(rgb));
			ctx->DrawTextLayout({ box.left + ox, box.top + oy }, layout.Get(), brush,
				D2D1_DRAW_TEXT_OPTIONS_NONE);
		}
	};

	// —— 19 个 painter。归一化坐标，y 向下；顺序即落盘值，只许往表尾加 ——
	// 0 箭头（系统指针）。点串借自 ToolMain::paintSelectIcon 那支，整体平移到方框里居中
	void paintArrow(IconKit& k)
	{
		static const float pts[][2] = {
			{ 0.235f, 0.075f }, { 0.235f, 0.831f }, { 0.411f, 0.643f },
			{ 0.529f, 0.924f }, { 0.670f, 0.867f }, { 0.553f, 0.585f }, { 0.764f, 0.585f },
		};
		std::vector<D2D1_POINT_2F> fig;
		for (auto& [nx, ny] : pts) fig.push_back(k.p(nx, ny));
		k.fillFigs({ fig }, inkC, k.halo(0.f));
	}
	// 1 手形（指向的那只手）：食指 + 手掌 + 拇指三块拼成一个 figure 集，一次衬底一次填色，
	// 拼接处不会露出白缝（衬底整份画完才轮到填色，填色把内部的缝全盖住了）
	void paintHand(IconKit& k)
	{
		static const float pts[][2] = {
			{ 0.36f, 0.12f }, { 0.50f, 0.12f }, { 0.50f, 0.40f }, { 0.62f, 0.42f },
			{ 0.74f, 0.50f }, { 0.76f, 0.58f }, { 0.71f, 0.64f }, { 0.71f, 0.80f },
			{ 0.64f, 0.90f }, { 0.33f, 0.90f }, { 0.25f, 0.78f }, { 0.25f, 0.52f }, { 0.36f, 0.46f },
		};
		std::vector<D2D1_POINT_2F> fig;
		for (auto& [nx, ny] : pts) fig.push_back(k.p(nx, ny));
		k.fillFigs({ fig }, inkC, k.halo(0.f));
	}
	// 2 文本输入（I 形光标）
	void paintIBeam(IconKit& k)
	{
		k.strokeFigs({
			{ k.p(0.32f, 0.10f), k.p(0.68f, 0.10f) },
			{ k.p(0.50f, 0.10f), k.p(0.50f, 0.90f) },
			{ k.p(0.32f, 0.90f), k.p(0.68f, 0.90f) },
			}, inkC, 0.055f * k.W(), k.halo(0.055f * k.W()));
	}
	// 3 十字
	void paintCross(IconKit& k)
	{
		k.strokeFigs({
			{ k.p(0.50f, 0.10f), k.p(0.50f, 0.90f) },
			{ k.p(0.10f, 0.50f), k.p(0.90f, 0.50f) },
			}, inkC, 0.10f * k.W(), k.halo(0.10f * k.W()));
	}
	// 鼠标轮廓 + 按下的那一格 + 点击处那两道弧线（左右键两枚共用）。
	// mirror = true 时左右翻转（右键）。四舍五入的坐标都写在归一化值里
	void paintClick(IconKit& k, const bool mirror)
	{
		auto mx = [&](const float nx) { return mirror ? 1.f - nx : nx; };
		// 轮廓（左右对称，镜像与否都是同一块）
		k.rrectStroke(0.30f, 0.08f, 0.70f, 0.92f, 0.16f, inkC, 0.05f * k.W(), k.halo(0.05f * k.W()));
		// 中缝：只画上半个（按键区）
		k.strokeFigs({ { k.p(0.50f, 0.08f), k.p(0.50f, 0.46f) } }, inkC, 0.045f * k.W());
		// 按下的那一格填色（红 = 左键，蓝 = 右键 —— 两枚分得开）
		k.rrectFill(std::min(mx(0.50f), mx(0.315f)), 0.095f,
			std::max(mx(0.50f), mx(0.315f)), 0.46f, 0.10f, mirror ? blueC : redC);
		// 点击处的两道弧线：左键在左上角外、右键镜像到右上角外
		k.arc(mirror ? 0.74f : 0.26f, 0.12f, 0.10f, mirror ? 270.f : 180.f, mirror ? 360.f : 270.f,
			inkC, 0.05f * k.W(), k.halo(0.05f * k.W()));
		k.arc(mirror ? 0.74f : 0.26f, 0.12f, 0.17f, mirror ? 270.f : 180.f, mirror ? 360.f : 270.f,
			inkC, 0.05f * k.W(), k.halo(0.05f * k.W()));
	}
	void paintClickLeft(IconKit& k) { paintClick(k, false); }
	void paintClickRight(IconKit& k) { paintClick(k, true); }
	// 6 忙碌：转圈（Windows 那个蓝色环）
	void paintBusy(IconKit& k)
	{
		k.arc(0.5f, 0.5f, 0.30f, 140.f, 440.f, blueC, 0.11f * k.W(), k.halo(0.11f * k.W()));
	}
	// 双向箭头。dir 是"正端"的单位方向；杆 + 两头的三角拼成一个 figure 集，
	// 一次衬底一次填色 —— 分开画的话两块的白衬底会在中缝互相切开
	void doubleArrow(IconKit& k, const float dx, const float dy, const UINT32 rgb)
	{
		const float len{ 0.34f };       // 杆的半长
		const float tip{ 0.10f };       // 箭头伸出杆端多少
		const float half{ 0.12f };      // 箭头底边的半宽
		const float shaft{ 0.045f };    // 杆的半宽
		auto c = std::pair{ 0.5f, 0.5f };
		auto end = std::pair{ 0.5f + dx * len, 0.5f + dy * len };
		auto back = std::pair{ 0.5f - dx * len, 0.5f - dy * len };
		auto px = -dy * half, py = dx * half;          // 底边的法向
		auto sx = -dy * shaft, sy = dx * shaft;        // 杆的法向
		auto tipAt = [&](const std::pair<float, float>& e, const float sgn) {
			return std::vector<D2D1_POINT_2F>{
				k.p(e.first + dx * tip * sgn, e.second + dy * tip * sgn),
					k.p(e.first + px * sgn, e.second + py * sgn),
					k.p(e.first - px * sgn, e.second - py * sgn),
			};
		};
		k.fillFigs({
			tipAt(end, 1.f), tipAt(back, -1.f),
			// 杆：一根细长的四边形
			{
				k.p(back.first + sx, back.second + sy),
				k.p(end.first + sx, end.second + sy),
				k.p(end.first - sx, end.second - sy),
				k.p(back.first - sx, back.second - sy),
			},
			}, rgb, k.halo(0.f));
	}
	void paintNS(IconKit& k) { doubleArrow(k, 0.f, 1.f, inkC); }
	void paintEW(IconKit& k) { doubleArrow(k, 1.f, 0.f, inkC); }
	// 9 东南（东北—西南双向）：FSCapture 里这一档管"往对角方向拉"
	void paintNESW(IconKit& k) { doubleArrow(k, 0.7071f, -0.7071f, inkC); }
	// 10 西南（西北—东南双向）
	void paintNWSE(IconKit& k) { doubleArrow(k, 0.7071f, 0.7071f, inkC); }
	// 11 全部（四向）
	void paintAll(IconKit& k)
	{
		const float len{ 0.30f }, tip{ 0.10f }, half{ 0.11f }, shaft{ 0.045f };
		std::vector<std::vector<D2D1_POINT_2F>> figs;
		auto head = [&](const float tx, const float ty, const float dx, const float dy) {
			figs.push_back({
				k.p(tx, ty),
				k.p(tx - dx * tip - dy * half, ty - dy * tip + dx * half),
				k.p(tx - dx * tip + dy * half, ty - dy * tip - dx * half),
				});
		};
		head(0.5f, 0.5f - len - tip, 0.f, -1.f);
		head(0.5f, 0.5f + len + tip, 0.f, 1.f);
		head(0.5f - len - tip, 0.5f, -1.f, 0.f);
		head(0.5f + len + tip, 0.5f, 1.f, 0.f);
		// 两根杆：竖、横
		figs.push_back({ k.p(0.5f - shaft, 0.5f - len), k.p(0.5f + shaft, 0.5f - len),
			k.p(0.5f + shaft, 0.5f + len), k.p(0.5f - shaft, 0.5f + len) });
		figs.push_back({ k.p(0.5f - len, 0.5f - shaft), k.p(0.5f + len, 0.5f - shaft),
			k.p(0.5f + len, 0.5f + shaft), k.p(0.5f - len, 0.5f + shaft) });
		k.fillFigs(figs, inkC, k.halo(0.f));
	}
	// 12 信息：蓝底白 i（圆点 + 竖条，几何画法，不依赖字体）
	void paintInfo(IconKit& k)
	{
		k.disc(0.5f, 0.5f, 0.38f, blueC);
		k.disc(0.5f, 0.28f, 0.05f, 0xFFFFFF);
		k.rrectFill(0.445f, 0.38f, 0.555f, 0.72f, 0.045f, 0xFFFFFF);
	}
	// 13 问题：蓝底白 ?
	void paintQuestion(IconKit& k)
	{
		k.disc(0.5f, 0.5f, 0.38f, blueC);
		k.text(L"?", 0xFFFFFF, 0.62f);
	}
	// 14 错误：红底白 ×
	void paintError(IconKit& k)
	{
		k.disc(0.5f, 0.5f, 0.38f, redC);
		k.strokeFigs({
			{ k.p(0.34f, 0.34f), k.p(0.66f, 0.66f) },
			{ k.p(0.66f, 0.34f), k.p(0.34f, 0.66f) },
			}, 0xFFFFFF, 0.09f * k.W());
	}
	// 15 确定：绿底白 ✓
	void paintOk(IconKit& k)
	{
		k.disc(0.5f, 0.5f, 0.38f, greenC);
		k.strokeFigs({
			{ k.p(0.30f, 0.52f), k.p(0.44f, 0.66f), k.p(0.70f, 0.36f) },
			}, 0xFFFFFF, 0.09f * k.W());
	}
	// 16 取消：白底红圈红 ×（与"错误"的实底版分得开）
	void paintCancel(IconKit& k)
	{
		k.disc(0.5f, 0.5f, 0.36f, 0xFFFFFF);
		k.disc(0.5f, 0.5f, 0.36f, redC, 0.06f * k.W());
		k.strokeFigs({
			{ k.p(0.35f, 0.35f), k.p(0.65f, 0.65f) },
			{ k.p(0.65f, 0.35f), k.p(0.35f, 0.65f) },
			}, redC, 0.09f * k.W());
	}
	// 17 提示：灯泡 + 三道光线
	void paintBulb(IconKit& k)
	{
		k.disc(0.5f, 0.42f, 0.26f, yellowC);
		k.disc(0.5f, 0.42f, 0.26f, amberC, 0.035f * k.W());
		k.rrectFill(0.42f, 0.66f, 0.58f, 0.80f, 0.03f, amberC);
		const float w{ 0.045f * k.W() };
		k.strokeFigs({ { k.p(0.5f, 0.03f), k.p(0.5f, 0.10f) } }, amberC, w, k.halo(w));
		k.strokeFigs({ { k.p(0.20f, 0.12f), k.p(0.27f, 0.19f) } }, amberC, w, k.halo(w));
		k.strokeFigs({ { k.p(0.80f, 0.12f), k.p(0.73f, 0.19f) } }, amberC, w, k.halo(w));
	}
	// 18 笑脸
	void paintSmile(IconKit& k)
	{
		k.disc(0.5f, 0.5f, 0.38f, yellowC);
		k.disc(0.37f, 0.38f, 0.05f, inkC);
		k.disc(0.63f, 0.38f, 0.05f, inkC);
		k.arc(0.5f, 0.48f, 0.20f, 25.f, 155.f, inkC, 0.055f * k.W());
	}

	// painter 表：下标 = 落盘值（toolPin.icon.kind）。19 个：
	//   0~6   指针类：箭头 / 手形 / 文本输入 / 十字 / 左键 / 右键 / 忙碌
	//   7~11  大小类：南北 / 东西 / 东南 / 西南 / 全部
	//   12~18 信息类：信息 / 问题 / 错误 / 确定 / 取消 / 提示 / 笑脸
	// 语言包里的名字键（tool.iconName0~18）与这个顺序一一对应
	using Painter = void (*)(IconKit&);
	constexpr Painter painters[]{
		paintArrow, paintHand, paintIBeam, paintCross, paintClickLeft, paintClickRight, paintBusy,
		paintNS, paintEW, paintNESW, paintNWSE, paintAll,
		paintInfo, paintQuestion, paintError, paintOk, paintCancel, paintBulb, paintSmile,
	};
}

// 工具条 / 下拉用的样例：全不透明、无影子。样例格往往不是正方形（下拉项是宽扁的、
// 按钮可能被压窄），按"格子的短边取一块居中正方形"来画 —— 宽格左右留白、窄格上下留白，
// 图标永远等比，不会被拉变形（作者截图里"图标被压扁"就是按整格拉伸画出来的）
void ShapeIcon::paintSample(ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect, const int kind)
{
	if (kind < 0 || kind >= kindCount()) return;
	ComPtr<ID2D1SolidColorBrush> brush;
	Ling::D2D::get()->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0, 0), brush.GetAddressOf());
	if (!brush) return;
	// 占格率：图标直径 = 短边 × (1 - 2×inset)。0.08 让 30 高的列表项里图标约 25px、
	// 面板按钮里约 25px —— 回显是"当前选了哪个"的唯一凭据，撑满一点才认得清
	const float inset{ 0.08f };
	const float w = rect.right - rect.left, h = rect.bottom - rect.top;
	const float side{ std::min(w, h) * (1.f - 2.f * inset) };
	const float ox{ rect.left + (w - side) / 2.f }, oy{ rect.top + (h - side) / 2.f };
	IconKit k{ ctx, brush.Get(), D2D1::RectF(ox, oy, ox + side, oy + side),
		1.f, false, 0.f, 0.f, 1.f };
	painters[kind](k);
}

ShapeIcon::ShapeIcon(Canvas* win) : ShapeBase(win)
{
	auto toolSub = win->getToolSub();
	kind = std::clamp(toolSub->iconKind, 0, kindCount() - 1);
	// 大小取图标工具自己的滑块值（物理像素）。与别的工具共用一张值域表（ToolSub 的 sliderCfgs）
	size = toolSub->getShapeSliderVal(L"icon");
	opacity = std::clamp(toolSub->iconOpacity, 20, 100);
	hasShadow = toolSub->iconShadow;
	Ling::D2D::get()->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0, 0), brush.GetAddressOf());
}

std::unique_ptr<ShapeBase> ShapeIcon::clone(const float dx, const float dy, Canvas* target) const
{
	return cloneSelf(*this, dx, dy, target);
}

std::unique_ptr<ShapeBase> ShapeIcon::snapshot() const
{
	return snapshotSelf(*this);
}

void ShapeIcon::fixupCopy()
{
	// 画刷重建一份：ComPtr 拷过来是同一支，改一方的颜色会连另一方一起改
	if (brush) {
		auto color = brush->GetColor();
		Ling::D2D::get()->deviceContext->CreateSolidColorBrush(color, brush.ReleaseAndGetAddressOf());
	}
}

void ShapeIcon::translate(const float dx, const float dy)
{
	cx += dx;
	cy += dy;
}

void ShapeIcon::applyStyle()
{
	auto toolSub = win->getToolSub();
	size = toolSub->getShapeSliderVal(L"icon");
	opacity = std::clamp(toolSub->iconOpacity, 20, 100);
	hasShadow = toolSub->iconShadow;
}

void ShapeIcon::applyToolStyle()
{
	kind = std::clamp(win->getToolSub()->iconKind, 0, kindCount() - 1);
}

void ShapeIcon::mouseDown(const float x, const float y)
{
	// 首次创建（hoverDraggerIndex == -1）：把落点定下来
	if (hoverDraggerIndex == -1) {
		cx = x;
		cy = y;
		hoverDraggerIndex = 0;
		win->refresh();
	}
	pressX = x;
	pressY = y;
}

void ShapeIcon::mouseMove(const float x, const float y)
{
	// 只有"方框内"一个命中档：拖它就是挪位置
	hoverDraggerIndex = size > 0.f
		&& x >= cx - size / 2 - draggerSize && x <= cx + size / 2 + draggerSize
		&& y >= cy - size / 2 - draggerSize && y <= cy + size / 2 + draggerSize ? 0 : -1;
}

void ShapeIcon::mouseDrag(const float x, const float y)
{
	if (hoverDraggerIndex != 0) return;
	cx += x - pressX;
	cy += y - pressY;
	pressX = x;
	pressY = y;
	win->refresh();
}

bool ShapeIcon::mouseWheel(const float x, const float y, const short delta)
{
	// Alt + 滚轮 = 调透明度（一格 5%）。普通滚轮 = 调大小（与序号的圆、矩形的线宽同一套）
	if (GetKeyState(VK_MENU) & 0x8000) {
		const int before = opacity;
		opacity = std::clamp(opacity + (delta > 0 ? 5 : -5), 20, 100);
		if (opacity == before) return false;
		// 落盘 + 面板上的透明度滑块跟着走（面板开着才有得同步）
		Setting::get()->setToolNum(L"icon", L"opacity", (float)opacity);
		auto toolSub = win->getToolSub();
		toolSub->iconOpacity = opacity;
		if (toolSub->opacitySlider) toolSub->opacitySlider->setValue((float)opacity);
		win->refresh();
		return true;
	}
	const auto before = size;
	if (delta < 0) size--;
	else size++;
	size = win->getToolSub()->setShapeSliderVal(L"icon", size);
	if (size == before) return false;   //夹回原值（已经顶到值域的头）→ 什么都没变
	win->refresh();
	return true;
}

void ShapeIcon::setCursor()
{
	if (hoverDraggerIndex >= 0) SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
}

void ShapeIcon::paint(ID2D1DeviceContext* ctx)
{
	if (size <= 0.f || !brush) return;
	D2D1_RECT_F box{ cx - size / 2.f, cy - size / 2.f, cx + size / 2.f, cy + size / 2.f };
	const float a = opacity / 100.f;
	// 影子那一次先行：同一份 painter 拿"全黑"的调色板再跑一遍，往右下错两像素。
	// 逐个图元画两遍比离屏合成便宜得多，也省掉一份"影子专用几何"要同步的负担
	if (hasShadow) {
		IconKit k{ ctx, brush.Get(), box, a, true, 2.f * win->getDpi(), 2.f * win->getDpi(), win->getDpi() };
		painters[kind](k);
	}
	IconKit k{ ctx, brush.Get(), box, a, false, 0.f, 0.f, win->getDpi() };
	painters[kind](k);
}

bool ShapeIcon::getShapeBounds(D2D1_RECT_F& out) const
{
	if (size <= 0.f) return false;
	// 影子往右下探出两像素，框上也让它占上 —— 否则迷你条 / 选中框会把影子裁在外面
	const float half = size / 2.f + (hasShadow ? 2.f * win->getDpi() : 0.f);
	out = D2D1::RectF(cx - half, cy - half, cx + half, cy + half);
	return true;
}
