#include "pch.h"
#include <include/Ling.h>
#include "CapLong.h"
#include "WinCap.h"
#include "CutMask.h"
#include "WinPin.h"
#include "../Tool/ToolLong.h"
#include "../App.h"
#include "../Util.h"
#include "../Lang.h"
#include "../Setting.h"
using namespace Microsoft::WRL;

namespace {
    constexpr UINT scrollMsgId = 18;
    constexpr UINT scrollEndMsgId = 19;
    constexpr int stripSize = 100;  // 匹配比较用的条带长度（沿滚动轴取）
    // 按这个次数还滚不动，就换另一个方向再试。比"判定触底"用的 maxDismissTime 小得多：
    // 换方向要趁早，晚了用户已经盯着不动的界面等了好几秒
    constexpr int dirFlipAt = 3;
    // 连续这么多次滚不动才认为到底了。以前是 2 次，反馈里有明明还能滚就提示触底的：
    // 滚轮发出去之后目标窗口不一定跟着动 —— 惯性滚动还没停、页面在加载、
    // 或者鼠标底下那一层刚好不接收滚轮，多试几次就过去了。
    // 每次重试之间隔 500ms，多等几轮的代价只是到底时晚几秒出提示
    constexpr int maxDismissTime = 8;
    // 滚轮发出去之后等多久再抓屏：太短会抓到滚动动画还没走完（甚至还没开始）的
    // 中间帧，滚动量被误判成 0，导致最后一截内容没接上、成图偏短。
    // 浏览器 / Electron 这类目标是动画式滚动，要给它留足时间
    constexpr int scrollSettleMs = 250;
    // 两次滚轮之间的间隔。加上滚动动画的沉降等待，一整轮落在 500~800ms 这个区间里：
    // 再快会抓到动画没走完的中间帧，拼图错行；再慢长页面要滚半天
    constexpr int scrollIntervalMs = 350;
    // "这一帧没滚得动"之后，下一次重试之间的间隔。不必再等一整轮 —— 刚判过没动，
    // 再等 350ms 也不会动。重试变密之后，"触底"与"这一页根本不滚"两条判定都早点落地
    //（反馈里"只有一屏、误点了长截图，干等半天才进编辑界面"等的就是这一串重试）
    constexpr int dismissRetryMs = 120;
    // 手动模式的抓屏间隔。不发滚轮，所以没有沉降等待要留，可以比自动那一路密一些
    constexpr int manualPollMs = 300;
    // 抓到"帧在变但匹配不出滚动量"的帧时，多半是滚动动画还没停。此时先不急着发
    // 下一次滚轮，隔一会儿重新抓一帧等它停稳；最多连续等这么多次，避免一直卡住
    constexpr int settleRecheckMs = 250;
    constexpr int maxSettleRecheck = 2;
    // 成图边长上限，到了就收工（D2D 那边 16384 才是硬限，这里留了余量）。
    // capStep 拿它判"该停了"，makeStopText 拿它挑那句"太长了"，stopCap 拿它决定
    // 还要不要把成图交给编辑界面 —— 超限的图交过去只是一扇建不出位图的空窗
    constexpr int longLimit = 36000;
    // 底部条带兜底匹配的置信度门槛
    constexpr double bottomMatchMinRatio = 0.9;   // 最佳误差要低于 s=0 误差的这个比例才采信
    constexpr double bottomMatchMaxError = 40000; // 平均每像素灰度误差超过这个值视为根本没对上

    // 将 BGRA 像素条带转为灰度图
    std::vector<BYTE> toGrayscale(const BYTE* bgra, int width, int height, int stride)
    {
        std::vector<BYTE> gray(width * height);
        for (int y = 0; y < height; y++) {
            const BYTE* src = bgra + y * stride;
            BYTE* dst = gray.data() + y * width;
            for (int x = 0; x < width; x++) {
                dst[x] = (BYTE)((src[x * 4] * 114 + src[x * 4 + 1] * 587 + src[x * 4 + 2] * 299) / 1000);
            }
        }
        return gray;
    }

    // 比较时的采样步长（行列都隔一个取一个）。这一段是长截图里最烫的地方：外层把条带
    // 沿滚动轴滑一遍，内层是 strip × width 个像素 —— 一个 1080p 的选区，内层就是
    // 100 × 1920 × 近千个候选 ≈ 两亿次乘加，而且全跑在 UI 线程上（反馈里的"卡住 /
    // 按 esc 没反应"就是它把消息循环堵住了）。隔行隔列取四分之一，MSE 是平均值、
    // 少采一半像素几乎不改判定；阈值那边按 sampleStep 还原成原来的量纲，所以还成立
    constexpr int sampleStep = 2;

    // 在 gray1 中搜索与 gray2 最相似的偏移 y（MSE 匹配）
    // 用平均误差而非累积误差，避免比较行数不同时 y=0 占便宜：
    // 累积 SSD 在 y=0 比较 100 行、y=15 比较 85 行，前者"容错空间"大，
    // 当滚动量小且内容有平滑区域时，y=0 的总误差反而更低，导致误判为"没滚动"。
    int findMostSimilarY(const BYTE* gray1, int gray1H, const BYTE* gray2, int gray2H, int width)
    {
        int searchH = gray1H - gray2H + 1;
        if (searchH <= 0) return 0;
        int rowsCompared = 0;
        for (int row = 0; row < gray2H; row += sampleStep) ++rowsCompared;
        if (rowsCompared <= 0) return 0;
        double minAvgError = DBL_MAX;
        int bestY = 0;
        for (int y = 0; y < searchH; y++) {
            double error = 0.0;
            // 已经超过当前最优的候选不必再比（后面的平方和只会更大）—— 绝大多数候选
            // 都在头几行就出局，这一条比采样本身省得还多
            const double limit = minAvgError * rowsCompared / sampleStep;
            bool worse = false;
            for (int row = 0; row < gray2H && !worse; row += sampleStep) {
                const BYTE* row1 = gray1 + (y + row) * width;
                const BYTE* row2 = gray2 + row * width;
                for (int x = 0; x < width; x += sampleStep) {
                    const int diff = (int)row1[x] - (int)row2[x];
                    error += diff * diff;
                    if (error > limit) { worse = true; break; }
                }
            }
            if (worse) continue;
            double avgError = error * sampleStep / rowsCompared;
            if (avgError < minAvgError) {
                minAvgError = avgError;
                bestY = y;
            }
        }
        return bestY;
    }

    // 横向滚动用：取 [x0, x0+bandW) 这一竖带，转成"一行是原图一列"的灰度图。
    // 转置之后竖向那套按行匹配的函数可以原样复用，行号就是原图的列号
    std::vector<BYTE> toGrayscaleColumn(const BYTE* bgra, int imgH, int x0, int bandW, int rowPix)
    {
        std::vector<BYTE> gray((size_t)imgH * bandW);
        for (int k = 0; k < bandW; ++k) {
            BYTE* dst = gray.data() + (size_t)k * imgH;
            for (int y = 0; y < imgH; ++y) {
                const BYTE* src = bgra + (size_t)y * rowPix + (size_t)(x0 + k) * 4;
                dst[y] = (BYTE)((src[0] * 114 + src[1] * 587 + src[2] * 299) / 1000);
            }
        }
        return gray;
    }

    // 判断两帧是否完全相同（内存比较，快）
    bool framesDiffer(const std::vector<BYTE>& a, const std::vector<BYTE>& b)
    {
        if (a.size() != b.size()) return true;
        return memcmp(a.data(), b.data(), a.size()) != 0;
    }

    // 用新旧两帧"底部条带"反推滚动量：滚动 s 像素后，新帧底部条带的前 stripH-s 行
    // 还是旧帧底部条带里的内容（整体上移了 s 行），最后 s 行才是新滚进来的内容。
    // 顶部条带是纯色/空白、常规匹配不上时用它兜底 —— 页面底部的真实内容往往在这里。
    // 返回滚动量 s；0 表示没对上（比如整条都是新内容，或条带太"平"匹配不可信）。
    int findScrollByBottomStrip(const BYTE* grayOld, const BYTE* grayNew, int width, int stripH)
    {
        double minAvgError = DBL_MAX;
        double avgAtZero = DBL_MAX;
        int bestS = 0;
        for (int s = 0; s < stripH; s++) {
            int rows = stripH - s; // 条带里还能和旧帧对上的行数
            int rowsCompared = 0;
            for (int r = 0; r < rows; r += sampleStep) ++rowsCompared;
            if (rowsCompared <= 0) continue;
            double error = 0.0;
            // 同 findMostSimilarY：超过当前最优的候选直接放弃（见那里的说明）。
            // s == 0 时 limit 是 DBL_MAX，所以 avgAtZero 一定算得出来
            const double limit = minAvgError * rowsCompared / sampleStep;
            bool worse = false;
            for (int r = 0; r < rows && !worse; r += sampleStep) {
                const BYTE* row1 = grayOld + (size_t)(s + r) * width;
                const BYTE* row2 = grayNew + (size_t)r * width;
                for (int x = 0; x < width; x += sampleStep) {
                    int diff = (int)row1[x] - (int)row2[x];
                    error += diff * diff;
                    if (error > limit) { worse = true; break; }
                }
            }
            if (worse) continue;
            double avgError = error * sampleStep / rowsCompared;
            if (s == 0) avgAtZero = avgError;
            if (avgError < minAvgError) {
                minAvgError = avgError;
                bestS = s;
            }
        }
        if (bestS <= 0) return 0;
        // 纯色/空白时所有偏移的误差都差不多（严格小于比较会让 bestS 停在 0），
        // 这里再兜一道：最佳误差必须明显小于 s=0，且绝对误差不能太大
        if (minAvgError >= avgAtZero * bottomMatchMinRatio) return 0;
        if (minAvgError > bottomMatchMaxError) return 0;
        return bestS;
    }
}

CapLong::CapLong(WinCap* win) : win(win)
{
    horizontal = Setting::get()->getLongHorizontal();
    startCircleR *= win->dpi;
    auto d2d = Ling::D2D::get();
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), textBrush.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.68f), bgBrush.GetAddressOf());
    auto size{ startCircleR * 2 };
    layoutTextStart = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.start"), 16 * win->dpi, size, size);
    if (layoutTextStart) {
        DWRITE_TEXT_METRICS tm{};
        layoutTextStart->GetMetrics(&tm);
        startTextSize = { tm.width, tm.height };
    }
    // 工具条一开始就摆出来：手动 / 自动那个开关要在点"开始"之前就够得着，
    // 否则只能先滚一段再切，开头那截已经按自动的节奏拼好了
    makeTool();
}

CapLong::~CapLong()
{
}

void CapLong::dispose()
{
    win->killTimer(scrollMsgId);
    win->killTimer(scrollEndMsgId);
    if (tool) tool->close();
}

void CapLong::paint(ID2D1DeviceContext* ctx)
{
    paintImgPreview(ctx);
    if (isFinish) {
        auto borderRadius{ 4.f * win->dpi };
        ctx->FillRoundedRectangle(D2D1::RoundedRect(stopTextRect, borderRadius, borderRadius), bgBrush.Get());
        ctx->DrawTextLayout(stopTextPos, layoutTextEnd.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    else if (isShowStartBtn) {
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F((float)circleCenter.x, (float)circleCenter.y), startCircleR, startCircleR), bgBrush.Get());
        ctx->DrawTextLayout({ circleCenter.x - startTextSize.width / 2, circleCenter.y - startTextSize.height / 2 },
            layoutTextStart.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
}

void CapLong::setCursor()
{
    if (!isFinish && isShowStartBtn) {
        // 开始按钮跟着光标走，藏掉系统光标免得两个东西叠在一起
        SetCursor(NULL);
    }
    else {
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
    }
}

void CapLong::onMove(POINT pos)
{
    if (isFinish) {
        if (isShowStartBtn) {
            isShowStartBtn = false;
            win->refresh();
        }
        return;
    }
    circleCenter = pos;
    auto& r = win->cutMask->maskRect;
    if (pos.x > r.left && pos.x < r.right && pos.y > r.top && pos.y < r.bottom) {
        isShowStartBtn = true;
        win->refresh();
    }
    else {
        if (isShowStartBtn) {
            isShowStartBtn = false;
            win->refresh();
        }
    }
}

void CapLong::onUp(POINT pos)
{
    if (isScrolling || isFinish) return;
    if (isShowStartBtn) { //按下开始按钮
        isScrolling = true;
        win->hollowWin();
        // 手动模式要让用户真的够得着滚动条：只把选区抠成洞的话，滚动条多半在选区外面，
        // 那里仍盖着本窗口，鼠标根本落不到目标窗口上。整窗让出鼠标，工具条是独立窗口照旧可点
        if (manual) win->setMouseTransparent(true);
        firstStep(); //首次截图
    }
}

void CapLong::onTimerCB(UINT timerId)
{
    if (timerId == scrollMsgId) {
        if (manual) {
            // 手动模式：不发滚轮，只按固定间隔抓一帧看内容变了没有。
            // 滚动条怎么滚、滚多快全由用户决定，我们只负责把新出现的内容接上去
            win->killTimer(scrollMsgId);
            capStep();
            return;
        }
        POINT pt;
        GetCursorPos(&pt);
        auto tarHwnd = WindowFromPoint(pt);
        if (targetHwnd == nullptr) {
            targetHwnd = tarHwnd;
        }
        if (tarHwnd != targetHwnd) {
            // 光标没在截屏区域：这一次不滚。但定时器必须自己续上 —— 顶上那句
            // killTimer 已经把本轮的定时器撤了，直接 return 就再也没有人来 arm，
            // 整个滚动截图永久停在这儿（画面还在、进度还在，就是一动不动 = "卡住"）
            armScroll();
            return;
        }
        win->killTimer(scrollMsgId);
        INPUT input = { 0 };
        input.type = INPUT_MOUSE;
        // 横向滚是另一条消息（滚轮左右倾斜），正值是往右 —— 内容左移，新内容从右边进来
        input.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
        input.mi.mouseData = horizontal ? WHEEL_DELTA : -WHEEL_DELTA;
        SendInput(1, &input, sizeof(INPUT));
        win->setTimer(scrollSettleMs, scrollEndMsgId); //滚动开始
    }
    else if (scrollEndMsgId == timerId) {
        win->killTimer(scrollEndMsgId); //滚动完成
        capStep();
    }
}

void CapLong::firstStep()
{
    auto& maskRect = win->cutMask->maskRect;
    imgW = int(maskRect.right - maskRect.left);
    imgH = int(maskRect.bottom - maskRect.top);
    resultW = imgW;
    resultH = imgH;
    capStartPos.x = (int)maskRect.left;
    capStartPos.y = (int)maskRect.top;
    ClientToScreen(win->hwnd, &capStartPos);
    imgData = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);
    img1 = imgData;
    makeImgPreview();
    win->refresh();
    win->setTimer(88, scrollMsgId); //准备滚动
}

void CapLong::makeImgPreview()
{
    imgPreview.Reset();
    // 按成图尺寸算，而不是按单帧：横向的长图是往右长的，拿 imgW 当宽度会越缩越不对
    float previewScaleW = tool ? (float)tool->w / (float)resultW : 1.0f;
    // 横向长图按工具条宽缩放会得到一条几像素高的细带，什么都看不出来：
    // 高度缩到看不清时改成按高度定缩放，宁可预览比工具条宽
    if (previewScaleW * resultH < 24.f) previewScaleW = 24.f / (float)resultH;
    int previewW = (int)((float)resultW * previewScaleW);
    int previewH = (int)((float)resultH * previewScaleW);
    if (previewW > 0 && previewH > 0) {
        std::vector<BYTE> scaledData((size_t)previewW * 4 * previewH);
        for (int y = 0; y < previewH; y++) {
            int srcY = (int)((float)y / previewScaleW);
            if (srcY >= resultH) srcY = resultH - 1;
            for (int x = 0; x < previewW; x++) {
                int srcX = (int)((float)x / previewScaleW);
                if (srcX >= resultW) srcX = resultW - 1;
                // 行距是 resultW（横向拼过之后就不再等于单帧的 imgW 了）
                int srcIdx = (srcY * resultW + srcX) * 4;
                int dstIdx = (y * previewW + x) * 4;
                scaledData[dstIdx] = imgData[srcIdx];
                scaledData[dstIdx + 1] = imgData[srcIdx + 1];
                scaledData[dstIdx + 2] = imgData[srcIdx + 2];
                scaledData[dstIdx + 3] = imgData[srcIdx + 3];
            }
        }
        D2D1_BITMAP_PROPERTIES1 props = {
            .pixelFormat{D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)},
            .dpiX{96.0f}, .dpiY{96.0f}, .bitmapOptions{D2D1_BITMAP_OPTIONS_NONE}
        };
        Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(previewW, previewH), scaledData.data(), previewW * 4, props, imgPreview.GetAddressOf());
    }
}

int CapLong::findChangeStart(const std::vector<BYTE>& data)
{
    // 沿滚动轴从"新内容进来的那一头"开始扫：竖向往下滚，变化先出现在上面，
    // 横向往右滚，变化先出现在左边 —— 两边都是从 0 开始找第一个不一样的
    if (horizontal) {
        for (int x = 0; x < imgW; x++) {
            for (int y = 0; y < imgH; y++) {
                auto idx = (size_t)(y * imgW + x) * 4;
                if (img1[idx] != data[idx] || img1[idx + 1] != data[idx + 1] || img1[idx + 2] != data[idx + 2]) return x;
            }
        }
    }
    else {
        for (int y = 0; y < imgH; y++) {
            for (int x = 0; x < imgW; x++) {
                auto idx = (size_t)(y * imgW + x) * 4;
                if (img1[idx] != data[idx] || img1[idx + 1] != data[idx + 1] || img1[idx + 2] != data[idx + 2]) return y;
            }
        }
    }
    return -1;
}

int CapLong::matchShift(const std::vector<BYTE>& data)
{
    const int rowPix{ imgW * 4 };
    // strip 沿滚动轴取：竖向是行数，横向是列数
    const int axisSize = horizontal ? imgW : imgH;
    int strip = std::min(stripSize, axisSize - changeStart);
    if (strip <= 0) return 0;
    int shift{ 0 };
    if (horizontal) {
        // 竖带转置之后，匹配函数里的"行"就是原图的列，宽参数换成长度 imgH
        int bandW = imgW - changeStart;
        auto gray1 = toGrayscaleColumn(img1.data(), imgH, changeStart, bandW, rowPix);
        auto gray2 = toGrayscaleColumn(data.data(), imgH, changeStart, strip, rowPix);
        shift = findMostSimilarY(gray1.data(), bandW, gray2.data(), strip, imgH);
        if (shift == 0) {
            // 左端对不上（滚动区域左边是纯色 / 空白时常见），换成右端那条竖带反推
            auto gray1End = toGrayscaleColumn(img1.data(), imgH, imgW - strip, strip, rowPix);
            auto gray2End = toGrayscaleColumn(data.data(), imgH, imgW - strip, strip, rowPix);
            shift = findScrollByBottomStrip(gray1End.data(), gray2End.data(), imgH, strip);
        }
    }
    else {
        int img1StripH = imgH - changeStart;
        auto gray1 = toGrayscale(img1.data() + changeStart * rowPix, imgW, img1StripH, rowPix);
        auto gray2 = toGrayscale(data.data() + changeStart * rowPix, imgW, strip, rowPix);
        shift = findMostSimilarY(gray1.data(), img1StripH, gray2.data(), strip, imgW);
        if (shift == 0) {
            // 顶部条带没对上：可能滚动区域顶部是纯色/空白（比如页面底部的留白），
            // 换用新帧底部的条带再反推一次滚动量
            auto gray1Bottom = toGrayscale(img1.data() + (imgH - strip) * rowPix, imgW, strip, rowPix);
            auto gray2Bottom = toGrayscale(data.data() + (imgH - strip) * rowPix, imgW, strip, rowPix);
            shift = findScrollByBottomStrip(gray1Bottom.data(), gray2Bottom.data(), imgW, strip);
        }
    }
    return shift;
}

void CapLong::stitch(const std::vector<BYTE>& data, const int shift)
{
    const int rowPix{ imgW * 4 };
    // 新帧里从 changeStart 到末端的这一段是"还没接上去的"，接到结果的这一头：
    // 竖向接在底部（按行搬），横向接在右侧（按行的尾巴搬）
    if (horizontal) {
        int paintStart = resultW - (imgW - shift - changeStart);
        int newResultW = paintStart + (imgW - changeStart);
        int newRowPix = newResultW * 4;
        // 旧结果的行距是 resultW*4，第二次拼接起就不再等于单帧的 rowPix 了
        int oldRowPix = resultW * 4;
        std::vector<BYTE> newResult((size_t)newRowPix * imgH);
        for (int y = 0; y < imgH; y++) {
            CopyMemory(newResult.data() + (size_t)y * newRowPix, imgData.data() + (size_t)y * oldRowPix, oldRowPix);
            CopyMemory(newResult.data() + (size_t)y * newRowPix + paintStart * 4,
                data.data() + (size_t)y * rowPix + changeStart * 4, (size_t)(imgW - changeStart) * 4);
        }
        imgData = std::move(newResult);
        resultW = newResultW;
    }
    else {
        int paintStart = resultH - (imgH - shift - changeStart);
        int newResultH = paintStart + (imgH - changeStart);
        std::vector<BYTE> newResult((size_t)rowPix * newResultH);
        // 拷贝旧结果
        CopyMemory(newResult.data(), imgData.data(), imgData.size());
        // 拷贝新截图从 changeStart 到底部的内容
        for (int row = 0; row < imgH - changeStart; row++) {
            CopyMemory(newResult.data() + (size_t)(paintStart + row) * rowPix,
                data.data() + (size_t)(changeStart + row) * rowPix, rowPix);
        }
        imgData = std::move(newResult);
        resultH = newResultH;
    }
}

void CapLong::flipDir()
{
    horizontal = !horizontal;
    dirFlipped = true;
    // 变化起点的含义跟着轴变了，得重新找
    firstCheck = true;
    changeStart = -1;
    dismissTime = 0;
    settleRecheckCount = 0;
    // 已经拼出来的那一截是沿另一个轴排的：resultW / resultH 的含义和 imgData 的行距全变了，
    // 几何对不上，只能丢掉、拿当前这一帧重新起头（换向都发生在"滚不动"那一步，此时 img1 就是最新一帧）
    // ⚠️ 所以换向**只在一次都没拼成过的时候**才允许发生 —— 那条守则在 countDismiss 里，
    // 这里丢掉的是"结果图"，拼过东西之后再走这条路就等于把用户已经截到的内容扔了
    imgData = img1;
    resultW = imgW;
    resultH = imgH;
    makeImgPreview();
    win->refresh();
    armScroll();
}

void CapLong::armScroll()
{
    // 手动模式没有"滚轮沉降"要等，抓屏可以密一些；自动那一路要把沉降时间算进去。
    // 正在"滚不动"的重试里（dismissTime > 0）走更短的间隔，见 dismissRetryMs
    if (!manual && dismissTime > 0) {
        win->setTimer(dismissRetryMs, scrollMsgId);
        return;
    }
    win->setTimer(manual ? manualPollMs : scrollIntervalMs, scrollMsgId);
}

// 手动模式下"这一帧没变化"是常态：用户可能正拖着滚动条、也可能停手在看。
// 只有自动模式才靠它判触底（我们自己在发滚轮，滚不动就说明到底了）
void CapLong::countDismiss()
{
    if (manual) {
        armScroll();
        return;    // 已自行续上，调用方直接返回
    }
    dismissTime++;
    // ⚠️ 已经拼进过内容之后**绝不能再换方向**。换向要把几何整个重来（resultW / resultH 的
    // 含义、imgData 的行距都跟着轴变），已经拼好的那一整段接不上，只能丢掉 ——
    // 于是成图只剩最后一帧。而"往另一个方向也滚不动"是必然的（页面本就是竖向滚的），
    // 结果是任何一次正常的竖向长截图，滚到底之后都会被换向 + 丢弃，
    // 最后交出去的是一张单屏图 —— 反馈里"经常只截取了最后一屏"就是这么来的
    const bool stitched{ resultH != imgH || resultW != imgW };
    if (!stitched && dismissTime > dirFlipAt && !dirFlipped) { flipDir(); return; }
    if (dismissTime > maxDismissTime) { stopCap(true); return; }
    armScroll();
    return;
}

void CapLong::capStep()
{
    auto data = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);
    // 检测滚动区域：首次时找出前后两帧的像素差异边界
    if (firstCheck) {
        changeStart = findChangeStart(data);
        if (changeStart == -1) {
            // 没有检测到变化，可能滚动未生效
            countDismiss();
            return;
        }
        firstCheck = false;
    }
    int shift = matchShift(data);
    if (shift == 0) { // 未检测到滚动
        if (framesDiffer(data, img1)) {
            // 帧在变但匹配不出滚动量：多半是滚动动画还没停、或页面还在加载。
            // 这时不急着判"滚不动"，隔一会儿重抓一帧等它停稳，最多等几次再放弃
            if (settleRecheckCount < maxSettleRecheck) {
                settleRecheckCount++;
                win->setTimer(settleRecheckMs, scrollEndMsgId);
                return;
            }
        }
        settleRecheckCount = 0;
        countDismiss();
        return;
    }
    dismissTime = 0;
    settleRecheckCount = 0;
    stitch(data, shift);
    img1 = data;
    if (resultW > longLimit || resultH > longLimit) { stopCap(true); return; }
    makeImgPreview();
    win->refresh();
    armScroll(); //准备下次滚动
}

void CapLong::makeTool()
{
    tool = std::make_unique<ToolLong>(win);
    // 尺寸在 ToolLong 构造里算好了，这里只定位；两者都要在建窗口之前设好
    layoutTool();
    tool->createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
}

void CapLong::layoutTool()
{
    if (!tool) return;
    // 宽高一律现问 tool 要，不再按 dpi 自己算：DPI 变化后工具条会重算尺寸再回头调这里
    auto toolW{ tool->w };
    POINT pos{ 0,0 };
    auto& cutMask = win->cutMask;
    if (win->w - cutMask->maskRect.right - 2 * win->dpi < toolW) {
        pos.x = (LONG)(cutMask->maskRect.left - toolW - cutMask->strokeWidth - 2 * win->dpi);
    }
    else {
        pos.x = (LONG)(cutMask->maskRect.right + cutMask->strokeWidth + 2 * win->dpi);
    }
    pos.y = (LONG)(cutMask->maskRect.bottom - tool->h);
    ClientToScreen(win->hwnd, &pos);
    // 上面那两条规则在全屏选区时都会算出屏幕外的坐标：右边放不下，换到左侧又必然是负数。
    // 工具条连带它上方那条缩略图就整个跑到屏幕外 —— 既看不到进度，也点不到停止，
    // 只能干等它自己滚到底。所以最后一律夹进所在显示器的工作区
    RECT toolRect{ pos.x, pos.y, pos.x + (LONG)toolW, pos.y + (LONG)tool->h };
    MONITORINFO mi{ sizeof(MONITORINFO) };
    GetMonitorInfo(MonitorFromRect(&toolRect, MONITOR_DEFAULTTONEAREST), &mi);
    pos.x = (std::max)(pos.x, mi.rcWork.left);
    pos.y = (std::max)(pos.y, mi.rcWork.top);
    pos.x = (std::min)(pos.x, mi.rcWork.right - (LONG)toolW);
    pos.y = (std::min)(pos.y, mi.rcWork.bottom - (LONG)tool->h);
    tool->setPosition(pos.x, pos.y);
}

void CapLong::paintImgPreview(ID2D1DeviceContext* ctx)
{
    if (!imgPreview || !tool) return;
    auto bitmapSize = imgPreview->GetPixelSize();
    float drawW = (float)bitmapSize.width;
    float drawH = (float)bitmapSize.height;
    POINT pos{ tool->x, tool->y - (int)drawH - (int)(2 * win->dpi) };
    ScreenToClient(win->hwnd, &pos);
    D2D1_RECT_F destRect = D2D1::RectF((float)pos.x, (float)pos.y, pos.x + drawW, pos.y + drawH);
    ctx->DrawBitmap(imgPreview.Get(), destRect);
}

// 收工。手动模式下整窗是让出鼠标的，这里要收回来，否则成图之后连工具条都点不到
void CapLong::stopCap(bool reachedEnd)
{
    isFinish = true;
    isScrolling = false;
    win->killTimer(scrollMsgId);
    win->killTimer(scrollEndMsgId);
    win->restoreWin();
    win->setMouseTransparent(false);
    makeStopText(reachedEnd);
    win->refresh();
    // 自己滚到底停下的 = 这一轮图截完了：直接把成图交给编辑界面，用户不必再点一次什么。
    // 上面那句话只会闪一帧，但留着它有用 —— 交接没成（一帧都没抓到时 pin 自己会返回）
    // 或者图超了限时窗口本来就得留着，那正是唯一能给用户的交代。
    // 用户叫停那条路（ESC）不经过这里：它走 finish → pin，最后汇到的是同一扇编辑窗。
    // 超限的不交接：那张图 D2D 建不出位图（见 capStep 里同一道判断），交过去只是一扇空窗
    if (reachedEnd && !imgData.empty() && resultW <= longLimit && resultH <= longLimit) {
        pin();
        // 长图窗不在这里就地关：本函数是从定时器回调里一路进来的，就地 DestroyWindow
        // 会把正在跑的这段调用栈脚下的窗口抽掉。排一拍再关 —— 与 WinCap::onClosed 里
        // "句柄立即销毁、对象推迟到下一轮消息循环"是同一个做法
        Ling::App::get()->dq.TryEnqueue([]() {
            if (auto cap = WinCap::get()) cap->close();
        });
    }
}

// 用户叫停的（ESC / 工具条按钮）不显示"已触底"—— 那句话只在真的滚到底时才成立
void CapLong::makeStopText(bool reachedEnd)
{
    if (!reachedEnd) {
        layoutTextEnd = nullptr;
        return;
    }
    if (resultW > longLimit || resultH > longLimit) {
        layoutTextEnd = Ling::D2D::get()->makeTextLayout(
            Lang::get(horizontal ? L"long.tooWide" : L"long.tooLong"), 13 * win->dpi);
    }
    else {
        layoutTextEnd = Ling::D2D::get()->makeTextLayout(
            Lang::get(horizontal ? L"long.reachedEnd" : L"long.reachedBottom"), 13 * win->dpi);
    }
    if (!layoutTextEnd) return;
    DWRITE_TEXT_METRICS tm = {};
    layoutTextEnd->GetMetrics(&tm);
    auto& maskRect = win->cutMask->maskRect;
    auto halfX = maskRect.left + (maskRect.right - maskRect.left) / 2;
    auto halfW = tm.width / 2;
    float padding{ 8 * win->dpi };
    stopTextRect.left = halfX - halfW - padding;
    stopTextRect.top = maskRect.bottom - 30 * win->dpi - padding;
    stopTextRect.right = halfX + halfW + padding;
    stopTextRect.bottom = maskRect.bottom - padding;
    layoutTextEnd->SetMaxWidth(stopTextRect.right - stopTextRect.left);
    layoutTextEnd->SetMaxHeight(stopTextRect.bottom - stopTextRect.top);
    // 圆角矩形是按文本宽度加 padding 撑出来的，文本本身要摆回它的正中
    stopTextPos = { halfX - halfW, stopTextRect.top + (stopTextRect.bottom - stopTextRect.top - tm.height) / 2 };
}

void CapLong::copyToClipboard()
{
    if (imgData.empty()) return;
    Util::saveToClipboard(resultW, resultH, imgData.data());
}

bool CapLong::saveToFile()
{
    if (imgData.empty()) return false;
    auto path = Util::resolveSavePath(win->hwnd);
    if (path.empty()) return false;
    auto fmt = (Util::ImgFormat)Util::getSaveFormat();
    return Util::saveToFile(path, resultW, resultH, imgData.data(), fmt);
}

void CapLong::toggleMode()
{
    manual = !manual;
    // 拼接是按"前后两帧的内容"对齐的，与谁发的滚动无关，所以中途换模式不会把已拼好的
    // 那一截弄坏。要跟着换的只有一件事：手动模式得把鼠标让给底下的窗口，用户才拖得动滚动条
    if (isRunning()) win->setMouseTransparent(manual);
    if (tool) tool->refreshMode();
    win->refresh();
}

void CapLong::finish(bool toPin)
{
    if (isRunning()) stopCap(false);
    if (toPin && !imgData.empty()) pin();
}

// 收工，并把成图开到编辑界面上（工具条在、矩形预选）。这是滚动截图这一轮的终点 ——
// 与"框选截图 → 框完直接进编辑界面"（见 WinCap::onUp）殊途同归，用户看到的是同一种窗。
// 两条路都汇到这里：滚到底自动停（stopCap）、用户按 ESC 叫停（finish）
void CapLong::pin()
{
    if (imgData.empty()) return;
    if (isRunning()) stopCap(false);
    // 居中放置在主显示器
    auto monitor = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    GetMonitorInfo(monitor, &mi);
    auto& workArea = mi.rcWork;
    int screenW = workArea.right - workArea.left;
    int screenH = workArea.bottom - workArea.top;
    int posX = workArea.left + (screenW - std::min(resultW, screenW)) / 2;
    int posY = workArea.top + (screenH - std::min(resultH, screenH)) / 2;
    WinPin::initFromData(posX, posY, resultW, resultH, imgData, L"rect");
}
