#include "pch.h"
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeMosaicRect.h"

namespace {
	// 这一段词里有没有阿拉伯数字。只认半角 —— 识别结果里的全角数字极少见，
	// 真出现时按"没数字"放过去，比为了它放宽判据、误打一整段无关内容安全
	bool containsDigit(const std::wstring& text)
	{
		return std::any_of(text.begin(), text.end(),
			[](const wchar_t c) { return c >= L'0' && c <= L'9'; });
	}
	// 这一个字是不是阿拉伯数字（逐字判据，见 buildSmartMasks）。
	// text 只认长度 1 的：字符级框本来就一个字一项，多出来的（整段误传进来）一律不当数字
	bool isAsciiDigit(const std::wstring& text)
	{
		return text.size() == 1 && text[0] >= L'0' && text[0] <= L'9';
	}
	// 一个框是不是"几乎铺满整张图"。大模型那条 OCR 没有词框，只合成一个覆盖全图的
	// 假词（见 WinPin::startOcr），拿它当数字框就等于把整张图打掉 —— 这里把它挡掉。
	// 判据两维都超 0.9：正常的 OCR 词框再长也只是图里的一行字，不会两维都顶满
	bool looksLikeWholeImage(const D2D1_RECT_F& r, const float imgW, const float imgH)
	{
		if (imgW <= 0.f || imgH <= 0.f) return false;
		return (r.right - r.left) >= imgW * 0.9f && (r.bottom - r.top) >= imgH * 0.9f;
	}
}

ShapeMosaicRect::ShapeMosaicRect(Canvas* win, bool smartErase)
	: ShapeRectBase(win), mosaicPaint{ win, this }
{
	auto toolSub = win->getToolSub();
	auto d2d = Ling::D2D::get();
	// 马赛克没有颜色可选，这个半透明品红只在拖拽过程中当占位提示 ——
	// 松开鼠标后 buildMosaic / buildErase / buildSmartMasks 会算出真正的画刷把它替换掉
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xF00FF0, 0.38f), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	// "这一笔是智能擦除"由调用方给（橡皮擦工具的第三档，见 History::createShape）。
	// 不在这里读工具档位：马赛克工具的矩形档也造本类，光看 eraserMode 会把矩形马赛克
	// 也当成擦除。智能打码那条路也不看它 —— 只有 buildSmartArea 一个入口
	isErase = smartErase;
	// 这里置 isFill 不是"填充矩形工具"的意思：这块是整块填充、没有可摸的边框，
	// hitBody 因此按固定 2*dpi 宽判命中带，而不是按线宽 —— 与滑块笔刷尺寸无关（见基类）
	isFill = true;
}

ShapeMosaicRect::~ShapeMosaicRect()
{

}

std::unique_ptr<ShapeBase> ShapeMosaicRect::clone(const float dx, const float dy, Canvas* target) const
{
	return cloneSelf(*this, dx, dy, target);
}

std::unique_ptr<ShapeBase> ShapeMosaicRect::snapshot() const
{
	return snapshotSelf(*this);
}

void ShapeMosaicRect::fixupCopy()
{
	ShapeRectBase::fixupCopy();
	// mosaicPaint 里存着"我是哪一个 shape"（它按"画到自己为止"回读画面来算），拷过来
	// 还指着原件 —— 不换掉的话，新的一份会把原件自己也算进画面里
	mosaicPaint = ShapeMosaicPaint(win, this);
}

void ShapeMosaicRect::translate(const float dx, const float dy)
{
	ShapeRectBase::translate(dx, dy);
	// 挪了地方，原来那一块画面的马赛克就不对了 —— 照 mouseUp 那条路重算一遍。
	// 智能打码要铺的框是"这一笔的内容"，跟着一起挪；框本身不重算（重算会把
	// 用户手动挪过的那一笔又拽回识别结果上）
	for (auto& m : masks) {
		m.left += dx; m.right += dx; m.top += dy; m.bottom += dy;
	}
	resetMosaic();
	if (isErase) {
		buildErase();
		return;
	}
	buildMosaic();
}

// 马赛克块大小是从 strokeWidth 折算的，改线宽要按新块大小重新生成马赛克位图。
// 只在已经生成过时重建 —— 还没 mouseUp 的那一笔交回 paint 自己处理
void ShapeMosaicRect::applyStyle()
{
	strokeWidth = win->getToolSub()->getSliderVal();
	if (mosaicBrush) buildMosaic();
}

void ShapeMosaicRect::paint(ID2D1DeviceContext* ctx)
{
	// 旋转由基类统一叠（马赛克也能转），画完要把变换还回去 —— 后面还有别的东西要画
	auto prev{ setRotateTransform(ctx) };
	if (eraseBrush) {
		ctx->FillRectangle(rect, eraseBrush.Get());
	}
	else if (isSmart) {
		// 智能打码只铺在识别出来的数字框上。位图刷是照着**整个选区**生成的一张，
		// 各段数字因此落在同一套色块格子上 —— 一行数字打出来是齐的（"横平竖直"）。
		// 还没 mouseUp（masks 空、刷子还没建）时先用占位色把选区示意出来
		// 刷子还没建出来（拖拽中 / 建失败）就退回占位色，两种都是 D2D 画刷
		ID2D1Brush* fill = mosaicBrush ? static_cast<ID2D1Brush*>(mosaicBrush.Get())
			: static_cast<ID2D1Brush*>(brush.Get());
		if (masks.empty()) {
			ctx->FillRectangle(rect, fill);
		}
		else {
			for (const auto& m : masks) ctx->FillRectangle(m, fill);
		}
	}
	else if (mosaicBrush) {
		ctx->FillRectangle(rect, mosaicBrush.Get());
	}
	else {
		ctx->FillRectangle(rect, brush.Get());
	}
	ctx->SetTransform(prev);
}

// 几何一变，之前算好的马赛克就不对了，先扔掉退回占位色
void ShapeMosaicRect::mouseDrag(const float x, const float y)
{
	resetMosaic();
	ShapeRectBase::mouseDrag(x, y);
}

void ShapeMosaicRect::mouseUp(const float x, const float y)
{
	ShapeRectBase::mouseUp(x, y);
	// 几何定下来了才算马赛克 —— 这一步要把 GPU 像素读回内存，拖拽过程中每帧做太贵
	if (isErase) {
		buildErase();
		return;
	}
	if (isSmart) {
		buildSmartMasks();
		return;
	}
	buildMosaic();
}

void ShapeMosaicRect::resetMosaic()
{
	mosaicBrush.Reset();
	// 智能擦除的取样色不跟着每一次拖动重算：取样要走一遍 GPU 读回 + 直方图统计，
	// 而拖动是按帧来的。留着上一次的色，等 mouseUp 再取一次，比退回占位色好看得多
	if (!isErase) eraseBrush.Reset();
}

void ShapeMosaicRect::buildMosaic()
{
	mosaicBrush = mosaicPaint.makeMosaicBrush(rect, mosaicBlockSize());
}

// 块越大越糊。滑块调的是笔画粗细，顺带让粗笔画对应大色块，下限 6px 保证肉眼可见。
// 智能打码要对齐格子边界，所以这个数得单独取出来算（见 snapMasksToBlocks）
int ShapeMosaicRect::mosaicBlockSize() const
{
	return std::max(6, (int)std::round(strokeWidth / 3.0f));
}

void ShapeMosaicRect::buildErase()
{
	D2D1_COLOR_F color{};
	if (!mosaicPaint.sampleBgColor(rect, 4, color)) return;
	Ling::D2D::get()->deviceContext->CreateSolidColorBrush(color, eraseBrush.GetAddressOf());
	win->refresh();
}

// 一键智能打码的入口（工具条上那枚星形按钮）。
//
// 与"拖着框选一遍"的唯一区别是几何从哪来：这里是调用方给的一整块（整张底图），
// 框选那条是鼠标拖出来的。定几何 → 算数字框 → 建马赛克刷，三步与 mouseUp 那条一模一样，
// 所以画出来的东西、能不能撤销、拖动时跟不跟着走，全都与手画的一笔完全一致
void ShapeMosaicRect::buildSmartArea(const D2D1_RECT_F& area)
{
	isSmart = true;
	rect = area;
	// 椭圆那一套派生几何（cx/cy/rx/ry）由它重算 —— 本类是矩形用不上，但 rect 变了
	// 就该走一次，免得别的用它的地方拿到旧值
	syncFromRect();
	// 手柄的位置也是由 rect 派生的。不走 mouseDown 就没人建它，
	// 而这一笔提交之后是**选中态**，选中态要画手柄、要能拖
	makeDraggers();
	buildSmartMasks();
}

// 智能打码：挑出要打码的数字，只在这些框上铺马赛克。
//
// 框有两个来源，优先用**字符级**的那个：
//  ① win->getOcrChars()（内置模型的 rec 按 CTC 对齐算出来的，每个字一项）
//     —— 逐字判是不是数字，相邻的数字字符合并成一段。只有这样 "A1234567" 里的
//     A、"订单号:12345" 里的"订单号:"才不会被一起打掉（作者要的"精确对数字打码"）；
//  ② 字符级框拿不到时（系统 OCR / 大模型）回落到词框 —— 那只能说"这一段里有数字"，
//     整段打掉。宁可多打一点也不漏打，这也是换引擎之前的旧行为。
//
// 词框来自贴图窗口建窗时后台跑的那一遍整图识别（WinPin::startOcr），坐标已经换算成
// 标注坐标，与这里的 rect 同一套，可以直接比
void ShapeMosaicRect::buildSmartMasks()
{
	masks.clear();
	const auto sz = win->getImgSize();
	const float imgW = (float)sz.width, imgH = (float)sz.height;
	const int blockSize = mosaicBlockSize();
	// 位图是按"选区左上角"起铺的（见 ShapeMosaicPaint::renderBackground 的 origin），
	// 格子线因此落在 floor(rect.left) + n*块大小 上 —— 横向对齐就按这套格子算
	const float bx = std::floor(rect.left);

	// 两处共用一个"把框收成 mask"的收尾：夹到选区 → 退化就丢
	const auto pushMask = [this](const float lo, const float top, const float hi, const float bottom) {
		const D2D1_RECT_F r{ std::max(lo, rect.left), std::max(top, rect.top),
			std::min(hi, rect.right), std::min(bottom, rect.bottom) };
		// 夹完已经退化（这一段几乎整个在选区外）就跳过
		if (r.right - r.left < 1.f || r.bottom - r.top < 1.f) return;
		masks.push_back(r);
	};
	const auto& chars = win->getOcrChars();
	if (!chars.empty()) {
		// 横向对齐的两条口子（只往外扩）。这一步**必须在这里做完**，不能留给尾巴上的
		// snapMasksToBlocks：那一步是无条件往外扩的，会把下面刚夹好的边又重新扩回格子线 ——
		// 实测 A1234567 夹到 81.5 之后被它扩回 72，A 的右腿又糊了
		const auto snapDown = [&](const float v) {
			return bx + std::floor((v - bx) / (float)blockSize) * (float)blockSize;
		};
		const auto snapUp = [&](const float v) {
			return bx + std::ceil((v - bx) / (float)blockSize) * (float)blockSize;
		};
		// 一次把这一块画面读回内存：每个字都要按自己那一行数一份"墨的列投影"，
		// 逐字各读一趟太贵（一趟读回 = 一次 GPU 离屏重现 + 拷回）
		std::vector<BYTE> pixels;
		UINT32 pitch{ 0 }, pixW{ 0 }, pixH{ 0 };
		D2D1_POINT_2F origin{};
		const bool havePix = mosaicPaint.readback(rect, pixels, pitch, pixW, pixH, origin);
		// 逐字量"真实笔画"的左右边界。**数字和非数字都要量** ——
		// 非数字那几团墨正是相邻数字不许越过的界线（见下面的 midL / midR）
		std::vector<float> inkL(chars.size(), 0.f), inkR(chars.size(), 0.f);
		std::vector<bool> inkOk(chars.size(), false);
		if (havePix) {
			for (size_t i = 0; i < chars.size(); ++i) {
				const auto& ch = chars[i];
				const float h = std::max(ch.h, 1.f);
				// 连着几列空才算"两团墨之间"：字内笔画之间那点小缝最多两三列，
				// 字与字之间至少六七列，取字高的 10% 落在两者中间（14 号字约 2 列、36 号约 4 列）
				const int blank = std::max(2, (int)std::round(h * 0.10f));
				// 往外扩的上限：相邻两字贴得很紧时，靠它兜住"别一路吃过去"
				const int maxExtend = (int)std::round(h * 0.8f);
				int l{ 0 }, r{ 0 };
				if (mosaicPaint.inkRunInBand(pixels, pitch, pixW, pixH, origin,
					(int)std::round(ch.cx), (int)std::floor(ch.y), (int)std::ceil(ch.y + ch.h),
					blank, maxExtend, l, r)) {
					inkL[i] = (float)l;
					inkR[i] = (float)r;
					inkOk[i] = true;
				}
			}
		}
		for (size_t i = 0; i < chars.size(); ++i) {
			const auto& ch = chars[i];
			if (!isAsciiDigit(ch.text)) continue;
			const float h = std::max(ch.h, 1.f);
			// 识别框是贴着笔画量的，抗锯齿那半圈落在框外。左右各放宽一点字高的比例：
			// 既补上这半圈，也让相邻两段（"89" 与 "00"）的框真的搭上，不再留缝
			const float pad = std::max(1.f, h * 0.12f);
			// 量到笔画就按笔画来；量不出来（种子不在字上 / 底太干净没有对比）退回识别框
			float lo = ch.x - pad, hi = ch.x + ch.w + pad;
			// 邻居那团墨的一半。越过它就把邻字（A 的右腿、订单号:的冒号）糊掉了；
			// 而两团墨之间的中点必落在空档里，所以它不会把数字自己的边啃掉 ——
			// "数字边缘完整覆盖"不受影响
			float midL{ -1.f }, midR{ -1.f };
			if (inkOk[i]) {
				lo = inkL[i] - pad;
				hi = inkR[i] + pad;
				midL = (i > 0 && inkOk[i - 1]) ? (inkR[i - 1] + inkL[i]) / 2.f : -1.f;
				midR = (i + 1 < chars.size() && inkOk[i + 1])
					? (inkR[i] + inkL[i + 1]) / 2.f : -1.f;
			}
			// 往外补齐到格子边界：马赛克是整块铺的，框边落在半格上时那一格的平均色
			// 会把笔画淡淡映出来，人眼看着就是"边缘没盖住"
			lo = snapDown(lo);
			if (midL > 0.f && midL < inkL[i]) lo = std::max(lo, midL);
			hi = snapUp(hi);
			if (midR > 0.f && midR > inkR[i]) hi = std::min(hi, midR);
			pushMask(lo, ch.y, hi, ch.y + ch.h);
		}
	}
	else {
		for (const auto& word : win->getOcrWords()) {
			if (!containsDigit(word.text)) continue;
			// 大模型那条路只有一个覆盖全图的假词框（没有词框信息），见 looksLikeWholeImage
			if (looksLikeWholeImage(D2D1::RectF(word.x, word.y, word.x + word.w, word.y + word.h),
				imgW, imgH)) continue;
			// 词框这条路量不出笔画（拿不到峰值中心），只能按旧办法放宽一点
			const float pad = std::max(1.f, word.h * 0.12f);
			pushMask(word.x - pad, word.y, word.x + word.w + pad, word.y + word.h);
		}
	}
	// 合并/对齐之前先按阅读顺序排一下：mergeMasksByRow 靠"同一行的段必然连续"来切行，
	// 字符级框来自行内顺序，本来就是有序的；词框那条也由引擎保证有序。这里不做排序，
	// 但要在两段来源之间保持同一个假设 —— 所以两条路各自有序即可
	mergeMasksByRow();
	// 字符级那条路横向已经对齐过（而且是**夹着邻居的墨**对齐的），尾巴上这一步只再管纵向：
	// 这一趟是无条件往外扩的，让它碰横向就把上面夹好的边又扩回格子线上去了
	snapMasksToBlocks(blockSize, chars.empty());
	buildMosaic();
}

// 同一行里挨着的几段并成一段，顺带把这一行各框的上下边界统一。
//
// 为什么要并：识别是按"词"给的，"8900" 经常被切成 "89" + "00"，两个紧贴的框之间那
// 两三个像素的缝正好落在数字中间 —— 马赛克是铺在框上的，缝里就原样露出笔画，
// 看上去就是"数字边缘没盖全"。并成一段之后缝自然消失。
// 判据用横向间隙（小于 0.8 倍字高才并）：像 "A1234567" 与后面隔了一个词的 "8900"
// 间隙远大于字高，不会被并到一起
void ShapeMosaicRect::mergeMasksByRow()
{
	auto centerY = [](const D2D1_RECT_F& r) { return (r.top + r.bottom) / 2.f; };
	std::vector<D2D1_RECT_F> out;
	out.reserve(masks.size());
	for (size_t i = 0; i < masks.size();) {
		// 分组方式与 WinPin::applyOcrWords 切行时用的是同一个判据（中心差 < 0.6 倍字高算同一行），
		// 而 ocrWords 本来就是按阅读顺序排的，同一行必然是连续的一段
		size_t j = i + 1;
		float top = masks[i].top, bottom = masks[i].bottom;
		while (j < masks.size()) {
			const float ref = std::max(bottom - top, masks[j].bottom - masks[j].top);
			if (std::abs(centerY(masks[j]) - centerY(masks[i])) > ref * 0.6f) break;
			top = std::min(top, masks[j].top);
			bottom = std::max(bottom, masks[j].bottom);
			++j;
		}
		// 先整行统一上下边界，再按横向间隙并段。左边界不动 —— 那是各段自己的位置，
		// 数字按列排时列也就自然对齐了
		const float gapLimit = std::max(bottom - top, 1.f) * 0.8f;
		D2D1_RECT_F cur = masks[i];
		cur.top = top;
		cur.bottom = bottom;
		for (size_t k = i + 1; k < j; ++k) {
			D2D1_RECT_F next = masks[k];
			next.top = top;
			next.bottom = bottom;
			if (next.left - cur.right <= gapLimit) {
				cur.left = std::min(cur.left, next.left);
				cur.right = std::max(cur.right, next.right);
			}
			else {
				out.push_back(cur);
				cur = next;
			}
		}
		out.push_back(cur);
		i = j;
	}
	masks.swap(out);
}

// 把框的对边对到马赛克格子的边界上，只往外扩。
//
// 位图是按"选区左上角"起铺的（见 ShapeMosaicPaint::renderBackground 的 origin），
// 格子线因此落在 floor(rect.left) + n*块大小 上。框边界不落在格子线上时，边上那半格
// 的平均色会把笔画淡淡映出来 —— 人眼看着就是"边缘没盖住"。对齐之后整行数字的格子
// 也连成一套，横平竖直
//
// snapX：横向要不要一起对。**字符级那条路传 false** —— 它在算框的时候已经对过横向了，
// 而且是"对着格子往外扩、再夹到邻居那团墨的一半"这个顺序；这里再无条件扩一次会把夹好的
// 边又扩回格子线上（实测把 A 的右腿又糊掉一次，见 buildSmartMasks 里的注释）。
// 词框那条路没有笔画信息，横向只能靠这一步对齐，所以传 true
void ShapeMosaicRect::snapMasksToBlocks(const int blockSize, const bool snapX)
{
	if (blockSize <= 0) return;
	const float B = (float)blockSize;
	const float bx = std::floor(rect.left);
	const float by = std::floor(rect.top);
	for (auto& m : masks) {
		if (snapX) {
			m.left = std::max(rect.left, bx + std::floor((m.left - bx) / B) * B);
			m.right = std::min(rect.right, bx + std::ceil((m.right - bx) / B) * B);
		}
		m.top = std::max(rect.top, by + std::floor((m.top - by) / B) * B);
		m.bottom = std::min(rect.bottom, by + std::ceil((m.bottom - by) / B) * B);
	}
}
