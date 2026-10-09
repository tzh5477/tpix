#include "pch.h"
#include "Canvas.h"
#include "History.h"
#include "Shape/ShapeBase.h"
#include "ShapeMosaicPaint.h"

using Microsoft::WRL::ComPtr;

ShapeMosaicPaint::ShapeMosaicPaint(Canvas* win, ShapeBase* self) : win{ win }, self{ self }
{
}

Microsoft::WRL::ComPtr<ID2D1BitmapBrush> ShapeMosaicPaint::makeMosaicBrush(const D2D1_RECT_F bounds, const int expand)
{
	ComPtr<ID2D1BitmapBrush> result;
	if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return result;
	std::vector<BYTE> pixels;
	UINT32 pitch{ 0 }, width{ 0 }, height{ 0 };
	D2D1_POINT_2F origin{};
	if (!renderBackground(bounds, expand, pixels, pitch, width, height, origin)) return result;
	mosaicPixels(pixels.data(), pitch, width, height, expand);
	auto bitmapProps = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
	ComPtr<ID2D1Bitmap> bitmap;
	Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(width, height),
		pixels.data(), pitch, &bitmapProps, bitmap.GetAddressOf());
	if (!bitmap) return result;
	// NEAREST_NEIGHBOR 是关键：线性插值会把色块边界糊成渐变，就不像马赛克了
	auto bitmapBrushProps = D2D1::BitmapBrushProperties(
		D2D1_EXTEND_MODE_CLAMP,
		D2D1_EXTEND_MODE_CLAMP,
		D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
	auto brushProps = D2D1::BrushProperties();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateBitmapBrush(bitmap.Get(), &bitmapBrushProps, &brushProps, result.GetAddressOf());
	if (!result) return result;
	// 位图只覆盖包围盒那一小块，画刷默认从原点铺，要平移到包围盒左上角才对得上
	result->SetTransform(D2D1::Matrix3x2F::Translation(origin.x, origin.y));
	return result;
}

bool ShapeMosaicPaint::sampleBgColor(const D2D1_RECT_F bounds, const int ringPx, D2D1_COLOR_F& out)
{
	// expand 传 4：再窄就容易被抗锯齿边缘带偏，再宽会把相邻内容的颜色混进来
	std::vector<BYTE> pixels;
	UINT32 pitch{ 0 }, width{ 0 }, height{ 0 };
	D2D1_POINT_2F origin{};
	if (!renderBackground(bounds, ringPx, pixels, pitch, width, height, origin)) return false;

	// 选区在读到这块像素里的位置（可能被窗口边界裁过，所以下面仍然逐点判断）
	const int inLeft = (int)std::floor(bounds.left - origin.x);
	const int inTop = (int)std::floor(bounds.top - origin.y);
	const int inRight = (int)(std::ceil)(bounds.right - origin.x);
	const int inBottom = (int)(std::ceil)(bounds.bottom - origin.y);

	auto modal = [&](bool excludeInside) {
		std::unordered_map<UINT32, int> counter;
		int best{ 0 };
		UINT32 bestKey{ 0 };
		for (UINT32 y = 0; y < height; ++y) {
			auto row = pixels.data() + y * pitch;
			for (UINT32 x = 0; x < width; ++x) {
				bool inside = (int)x >= inLeft && (int)x < inRight && (int)y >= inTop && (int)y < inBottom;
				if (excludeInside && inside) continue;
				auto px = row + x * 4;
				// 全透明处没有颜色可言，别让它当上众数
				if (px[3] < 128) continue;
				// 键只装 RGB：alpha 恒为 255，拆开统计反而会把同一底色分成好几份
				UINT32 key = ((UINT32)px[2] << 16) | ((UINT32)px[1] << 8) | px[0];
				auto n = ++counter[key];
				if (n > best) {
					best = n;
					bestKey = key;
				}
			}
		}
		if (best == 0) return false;
		out = D2D1::ColorF(((bestKey >> 16) & 0xFF) / 255.f,
			((bestKey >> 8) & 0xFF) / 255.f, (bestKey & 0xFF) / 255.f);
		return true;
	};
	// 选区正好贴着窗口边时外圈被裁没了，退化成整块（含选区内）的众数 —— 纯色底上结果一致
	if (modal(true)) return true;
	return modal(false);
}

bool ShapeMosaicPaint::renderBackground(const D2D1_RECT_F bounds, const int expand, std::vector<BYTE>& pixels,
	UINT32& pitch, UINT32& width, UINT32& height, D2D1_POINT_2F& origin)
{
	if (win->getWidth() <= 0 || win->getHeight() <= 0) return false;
	if (!win->screenImg) return false;

	auto d2d = Ling::D2D::get();
	auto ctx = d2d->deviceContext.Get();

	// 四周各外扩 expand 像素再夹到图片范围内。
	// bounds 给的是"标注坐标"，而底图在标注坐标系里是从 imgOrigin 开始铺的那一块
	//（贴图窗口剪过一刀就有偏移，见 Canvas::imgOrigin），所以窗口范围要按它算
	const auto& io = win->imgOrigin;
	const int imgL = io.x, imgT = io.y;
	const int imgR = imgL + (int)win->getWidth(), imgB = imgT + (int)win->getHeight();
	int left = std::max(imgL, std::min((int)std::floor(bounds.left) - expand, imgR));
	int top = std::max(imgT, std::min((int)std::floor(bounds.top) - expand, imgB));
	int right = std::max(imgL, std::min((int)std::ceil(bounds.right) + expand + 1, imgR));
	int bottom = std::max(imgT, std::min((int)std::ceil(bounds.bottom) + expand + 1, imgB));
	if (left >= right || top >= bottom) return false;

	origin = { (float)left, (float)top };
	auto localSize = D2D1::SizeU((UINT32)(right - left), (UINT32)(bottom - top));

	D2D1_BITMAP_PROPERTIES1 targetProps{
		.pixelFormat{ D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED) },
		.dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_TARGET }
	};
	ComPtr<ID2D1Bitmap1> targetBitmap;
	HRESULT hr{ S_OK };
	hr = ctx->CreateBitmap(localSize, nullptr, 0, &targetProps, targetBitmap.GetAddressOf());
	if (FAILED(hr)) return false;

	// 底图 + 排在自己前面且没被撤销的 shape。平移变换让窗口坐标直接落到这块小位图里，
	// 各 shape 的 paint 不用知道自己被画到了别处
	ctx->SetTarget(targetBitmap.Get());
	ctx->SetTransform(D2D1::Matrix3x2F::Translation(-origin.x, -origin.y));
	ctx->BeginDraw();
	ctx->Clear(D2D1::ColorF(0, 0.0f));
	ctx->DrawBitmap(win->screenImg.Get(), D2D1::RectF((float)imgL, (float)imgT, (float)imgR, (float)imgB));
	for (auto& shape : win->history->shapes)
	{
		auto cur = shape.get();
		if (cur == self) break;
		cur->paint(ctx);
	}
	hr = ctx->EndDraw();
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	// 解绑，下面 CopyFromBitmap 才能把它当源读
	ctx->SetTarget(nullptr);
	if (FAILED(hr)) return false;

	// GPU 上的 target 位图不能直接 Map，得先拷到一块带 CPU_READ 的位图上
	D2D1_BITMAP_PROPERTIES1 cpuProps{
		.pixelFormat{ targetBitmap->GetPixelFormat() },
		.dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
	};
	ComPtr<ID2D1Bitmap1> cpuBitmap;
	hr = ctx->CreateBitmap(localSize, nullptr, 0, &cpuProps, cpuBitmap.GetAddressOf());
	if (FAILED(hr)) return false;

	hr = cpuBitmap->CopyFromBitmap(nullptr, targetBitmap.Get(), nullptr);
	if (FAILED(hr)) return false;

	D2D1_MAPPED_RECT mapped{};
	hr = cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped);
	if (FAILED(hr)) return false;

	pixels.resize((size_t)mapped.pitch * localSize.height);
	CopyMemory(pixels.data(), mapped.bits, pixels.size());
	cpuBitmap->Unmap();

	pitch = mapped.pitch;
	width = localSize.width;
	height = localSize.height;
	return true;
}

void ShapeMosaicPaint::mosaicPixels(BYTE* bits, UINT32 pitch, UINT32 width, UINT32 height, int blockSize)
{
	if (!bits || blockSize <= 0 || width == 0 || height == 0) return;

	for (UINT32 y = 0; y < height; y += blockSize) {
		UINT32 yEnd = std::min(y + (UINT32)blockSize, height);
		for (UINT32 x = 0; x < width; x += blockSize) {
			UINT32 xEnd = std::min(x + (UINT32)blockSize, width);
			unsigned long long bSum{ 0 }, gSum{ 0 }, rSum{ 0 }, aSum{ 0 }, count{ 0 };
			for (UINT32 yy = y; yy < yEnd; ++yy) {
				auto row = bits + yy * pitch;
				for (UINT32 xx = x; xx < xEnd; ++xx) {
					auto pixel = row + xx * 4;
					bSum += pixel[0];
					gSum += pixel[1];
					rSum += pixel[2];
					aSum += pixel[3];
					++count;
				}
			}
			if (count == 0) continue;
			BYTE b = (BYTE)(bSum / count);
			BYTE g = (BYTE)(gSum / count);
			BYTE r = (BYTE)(rSum / count);
			BYTE a = (BYTE)(aSum / count);
			for (UINT32 yy = y; yy < yEnd; ++yy) {
				auto row = bits + yy * pitch;
				for (UINT32 xx = x; xx < xEnd; ++xx) {
					auto pixel = row + xx * 4;
					pixel[0] = b;
					pixel[1] = g;
					pixel[2] = r;
					pixel[3] = a;
				}
			}
		}
	}
}
