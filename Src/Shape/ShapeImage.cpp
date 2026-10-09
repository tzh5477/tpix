#include "pch.h"
#include "ShapeImage.h"

using Microsoft::WRL::ComPtr;

ShapeImage::ShapeImage(Canvas* win) :ShapeRectBase(win)
{
	// 这是一整块实心内容，没有可摸的边框 —— 命中带按"填充图形"那条路算（见基类 hitBody）。
	// 顺带让滚轮调线宽那条路提前退出（它本来就只认 useToolStyle，这里再声明一次意图）
	isFill = true;
}

ShapeImage::~ShapeImage()
{
}

bool ShapeImage::setImage(const std::vector<BYTE>& px, const int w, const int h)
{
	if (w <= 0 || h <= 0 || px.size() < (size_t)w * 4 * h) return false;
	pixels = px;
	// 预算一份预乘的：D2D 的 DrawBitmap 只认 D2D1_ALPHA_MODE_PREMULTIPLIED 的位图，
	// 而剪贴板（PNG 走 WIC、DIB 直接搬）交出来的是 straight alpha。
	// 不预乘直接塞进去，半透明像素会按"颜色不变、alpha 当遮罩"重算一遍，
	// 边缘会泛出一圈不该有的亮边
	for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
		const auto a = pixels[i + 3];
		if (a == 255) continue;   // 不透明的占绝大多数（截图、24bpp DIB），跳过省一遍乘法
		pixels[i + 0] = (BYTE)(pixels[i + 0] * a / 255);
		pixels[i + 1] = (BYTE)(pixels[i + 1] * a / 255);
		pixels[i + 2] = (BYTE)(pixels[i + 2] * a / 255);
	}
	imgW = w;
	imgH = h;
	return makeBitmap();
}

bool ShapeImage::makeBitmap()
{
	bitmap.Reset();
	if (imgW <= 0 || imgH <= 0 || pixels.size() < (size_t)imgW * 4 * imgH) return false;
	D2D1_BITMAP_PROPERTIES1 props{};
	props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
	props.dpiX = 96.f;
	props.dpiY = 96.f;
	props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
	// CreateBitmap 会把像素拷进去，pixels 的生存期只到这一行为止（所以不必长期持有它给 D2D 用；
	// 留着是为了 clone 时能重建 —— 见头文件）
	return SUCCEEDED(Ling::D2D::get()->deviceContext->CreateBitmap(
		D2D1::SizeU((UINT32)imgW, (UINT32)imgH), pixels.data(), (UINT32)imgW * 4,
		&props, bitmap.GetAddressOf()));
}

void ShapeImage::placeAt(const float x, const float y, const float w, const float h)
{
	rect = D2D1::RectF(x, y, x + w, y + h);
	syncFromRect();
	makeDraggers();
}

void ShapeImage::paint(ID2D1DeviceContext* ctx)
{
	if (!bitmap) return;
	// 旋转由基类统一叠（图片也能转），画完要把变换还回去 —— 后面还有别的元素要画
	auto prev{ setRotateTransform(ctx) };
	// 源矩形取整张图、目标矩形取 rect：缩放就是 rect 被手柄改大了，位图跟着拉伸
	ctx->DrawBitmap(bitmap.Get(), rect, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
		D2D1::RectF(0.f, 0.f, (float)imgW, (float)imgH));
	ctx->SetTransform(prev);
}

void ShapeImage::mouseMove(const float x, const float y)
{
	ShapeRectBase::mouseMove(x, y);
	if (hoverDraggerIndex != -1) return;
	// 拖图片中间那一大片也该能整体搬动它。基类的矩形命中只认边框那一圈
	//（那是给空心矩形留的：一块实心区域把整片都吃掉的话，压在底下的别的标注就再也点不着了），
	// 而图片自己就是盖在最上层的那块内容，不存在"挡住底下"的问题
	auto p = unrotatePoint(D2D1::Point2F(x, y), rectCenter(), angle);
	if (p.x >= rect.left && p.x <= rect.right && p.y >= rect.top && p.y <= rect.bottom) {
		hoverDraggerIndex = HitBody;
	}
}

void ShapeImage::fixupCopy()
{
	// 位图是设备资源，复制出来的一份要自己重建（画刷同理，见 ShapeBase::cloneSelf）。
	// 源像素跟着拷贝构造一起过来了，照着再建一张就够 —— 基类那份 fixupCopy 只重建画刷，
	// 本类没有画刷，所以不必调它
	makeBitmap();
}

std::unique_ptr<ShapeBase> ShapeImage::clone(const float dx, const float dy, Canvas* target) const
{
	return cloneSelf(*this, dx, dy, target);
}

std::unique_ptr<ShapeBase> ShapeImage::snapshot() const
{
	return snapshotSelf(*this);
}
