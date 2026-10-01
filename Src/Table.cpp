#include "pch.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <onnxruntime_cxx_api.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Storage.Streams.h>
#include "Table.h"
#include "Lang.h"
#include "Ocr.h"
#include "Setting.h"

namespace {
	using namespace winrt;
	using namespace winrt::Windows::Foundation;
	using namespace winrt::Windows::Storage::Streams;
	using namespace winrt::Windows::Web::Http;

	// slanet-plus 的输入边长，模型里写死的，RapidTable 也是这个数
	constexpr int side{ 488 };
	constexpr std::wstring_view modelName{ L"slanet-plus.onnx" };
	constexpr std::wstring_view modelUrl{ L"https://www.modelscope.cn/models/RapidAI/RapidTable/resolve/master/slanet-plus.onnx" };
	// ImageNet 的均值 / 标准差，逐通道套。入参是 BGRA，取前三个就是 BGR ——
	// 模型就是拿 cv2（BGR）这么训出来的，换成 RGB 反而认不准
	constexpr float mean[3]{ 0.485f, 0.456f, 0.406f };
	constexpr float stdv[3]{ 0.229f, 0.224f, 0.225f };

	// 会话是全局一份：建一次要读模型、要预热，每次识别都重建太亏。
	// 下面这几个只在 recognize 里碰，而 recognize 全程持锁 —— 连点两次表格按钮也只是排队
	std::mutex mtx;
	std::unique_ptr<Ort::Env> env;
	std::unique_ptr<Ort::Session> session;
	// 模型的自定义元数据里给的 token 表，前后各加一个 sos / eos
	std::vector<std::string> tokens;
	size_t eosIdx{ 0 };

	// 一个格子。框是模型给的，单位是原图像素
	struct Cell
	{
		float x0{ 0 }, y0{ 0 }, x1{ 0 }, y1{ 0 };
		int row{ 0 }, col{ 0 };
		int rowSpan{ 1 }, colSpan{ 1 };
		std::wstring text;
	};

	std::filesystem::path modelPath()
	{
		return Setting::get()->getDataPath() / modelName;
	}

	bool downloadModel(const std::filesystem::path& path, std::wstring& err)
	{
		// 没网、被拦、磁盘写不进去，对用户来说都是同一句话
		err = Lang::get(L"ocr.tableNoModel");
		try {
			HttpClient client;
			auto buffer = client.GetBufferAsync(Uri{ modelUrl }).get();
			std::vector<BYTE> bytes(buffer.Length());
			DataReader::FromBuffer(buffer).ReadBytes(bytes);
			// 模型有 7M 多，比这小得多说明下回来的是个错误页
			if (bytes.size() < 1024 * 1024) return false;
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);
			// 先写临时文件再改名：下载中断不会留下一个半截的 .onnx，骗过下一次的存在检查
			auto tmp = path;
			tmp += L".downloading";
			{
				std::ofstream file{ tmp, std::ios::binary | std::ios::trunc };
				if (!file) return false;
				file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			}
			std::filesystem::rename(tmp, path, ec);
			return !ec;
		}
		catch (...) {
			return false;
		}
	}

	bool ensureSession(std::wstring& err)
	{
		if (session) return true;
		auto path = modelPath();
		if (!std::filesystem::exists(path) && !downloadModel(path, err)) return false;
		try {
			// 一次只认一张表，多开线程只是跟用户抢 CPU；日志只留错误级，别往控制台刷
			env.reset(new Ort::Env{ ORT_LOGGING_LEVEL_ERROR, "tpix" });
			Ort::SessionOptions opt;
			opt.SetIntraOpNumThreads(1);
			session.reset(new Ort::Session{ *env, path.c_str(), opt });
			// token 表在模型的自定义元数据里（一行一个）。它跟着模型走，不写死在代码里
			Ort::AllocatorWithDefaultOptions allocator;
			auto chars = session->GetModelMetadata().LookupCustomMetadataMapAllocated("character", allocator);
			if (!chars) {
				session.reset();
				err = Lang::get(L"ocr.tableFail");
				return false;
			}
			tokens.clear();
			tokens.emplace_back("sos");
			std::string_view all{ chars.get() };
			size_t pos{ 0 };
			while (pos < all.size()) {
				auto nl = all.find('\n', pos);
				auto one = all.substr(pos, nl == std::string_view::npos ? nl : nl - pos);
				if (!one.empty()) tokens.emplace_back(one);
				if (nl == std::string_view::npos) break;
				pos = nl + 1;
			}
			tokens.emplace_back("eos");
			eosIdx = tokens.size() - 1;
			return true;
		}
		catch (...) {
			session.reset();
			err = Lang::get(L"ocr.tableFail");
			return false;
		}
	}

	// 缩成 488×488 的 letterbox：长边缩到 488、短边按比例，右下补 0。
	// 补的是归一化之后的 0（不是黑）—— RapidTable 就是先归一化再贴进全 0 的底，顺序不能反。
	// 出参是 CHW 排布的三片连续内存
	void letterbox(const int w, const int h, BYTE* data, std::vector<float>& out)
	{
		const auto ratio = (float)side / (float)std::max(w, h);
		// 与 RapidTable 一致：截断，不四舍五入
		const int rw = std::max(1, (int)(w * ratio));
		const int rh = std::max(1, (int)(h * ratio));
		out.assign((size_t)3 * side * side, 0.f);
		for (int c = 0; c < 3; ++c) {
			auto plane = out.data() + (size_t)c * side * side;
			for (int y = 0; y < rh; ++y) {
				// 双线性：目标像素中心映射回原图，与 PIL / cv2 的缩放对齐方式一致
				const auto fy = std::clamp((y + 0.5f) * (h / (float)rh) - 0.5f, 0.f, (float)h - 1.f);
				const int y0 = (int)fy;
				const int y1 = std::min(y0 + 1, h - 1);
				const auto ay = fy - y0;
				for (int x = 0; x < rw; ++x) {
					const auto fx = std::clamp((x + 0.5f) * (w / (float)rw) - 0.5f, 0.f, (float)w - 1.f);
					const int x0 = (int)fx;
					const int x1 = std::min(x0 + 1, w - 1);
					const auto ax = fx - x0;
					auto px = [&](const int sx, const int sy) {
						return (float)data[((size_t)sy * w + sx) * 4 + c] / 255.f;
					};
					const auto top = px(x0, y0) * (1.f - ax) + px(x1, y0) * ax;
					const auto bottom = px(x0, y1) * (1.f - ax) + px(x1, y1) * ax;
					plane[(size_t)y * side + x] = (top * (1.f - ay) + bottom * ay - mean[c]) / stdv[c];
				}
			}
		}
	}

	// 跑一次模型。loc 是每步的格子框（8 个数：四个点的 x、y 交替），
	// prob 是每步的结构概率（每步 token 数那么多个）
	bool infer(const std::vector<float>& input, std::vector<float>& loc,
		std::vector<float>& prob, int& steps, std::wstring& err)
	{
		try {
			Ort::AllocatorWithDefaultOptions allocator;
			auto inName = session->GetInputNameAllocated(0, allocator);
			auto outName0 = session->GetOutputNameAllocated(0, allocator);
			auto outName1 = session->GetOutputNameAllocated(1, allocator);
			std::array<int64_t, 4> shape{ 1, 3, side, side };
			auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
			Ort::Value tensor = Ort::Value::CreateTensor<float>(memInfo,
				const_cast<float*>(input.data()), input.size(), shape.data(), shape.size());
			std::array<const char*, 1> inNames{ inName.get() };
			std::array<const char*, 2> outNames{ outName0.get(), outName1.get() };
			auto outs = session->Run(Ort::RunOptions{ nullptr }, inNames.data(), &tensor, 1,
				outNames.data(), outNames.size());
			// 两个输出哪个是框、哪个是结构，按最后一维认：8 个数一条的是框
			for (auto& val : outs) {
				auto info = val.GetTensorTypeAndShapeInfo();
				auto dims = info.GetShape();
				if (dims.size() != 3) continue;
				auto p = val.GetTensorData<float>();
				if (dims[2] == 8) {
					loc.assign(p, p + (size_t)dims[1] * 8);
					steps = (int)dims[1];
				}
				else {
					prob.assign(p, p + (size_t)dims[1] * dims[2]);
				}
			}
			return !loc.empty() && !prob.empty() && steps > 0;
		}
		catch (...) {
			err = Lang::get(L"ocr.tableFail");
			return false;
		}
	}

	// 结构是一串 HTML 片段。一个格子要么是一个 <td></td>（空格子），
	// 要么是 <td …> 开头、</td> 收尾 —— 这两种的格子框都在开头那一帧上
	bool isCellStart(const std::string& t)
	{
		return t == "<td" || t == "<td></td>";
	}

	int spanOf(const std::string& t)
	{
		// 形如 ' colspan="3"'
		auto q1 = t.find('"');
		if (q1 == std::string::npos) return 1;
		auto q2 = t.find('"', q1 + 1);
		if (q2 == std::string::npos) return 1;
		return std::max(1, atoi(t.substr(q1 + 1, q2 - q1 - 1).c_str()));
	}

	struct Decoded
	{
		std::vector<Cell> cells;
		// grid[r][c] = 格子下标，-1 是空位。合并单元格占下来的位置也填同一个下标
		std::vector<std::vector<int>> grid;
	};

	Decoded decode(const std::vector<float>& loc, const std::vector<float>& prob,
		const int steps, const int w, const int h)
	{
		Decoded out;
		const auto tokenNum = (int)tokens.size();
		const auto scale = (float)std::max(w, h);
		int row{ -1 };
		for (int i = 0; i < steps; ++i) {
			const auto p = prob.data() + (size_t)i * tokenNum;
			int best{ 0 };
			for (int k = 1; k < tokenNum; ++k) if (p[k] > p[best]) best = k;
			if (i > 0 && best == (int)eosIdx) break;   // eos：结构到头了
			if (best == 0 || best == (int)eosIdx) continue;   // sos / eos 不成字
			const auto& t = tokens[best];
			if (t == "<tr>") {
				++row;
				continue;
			}
			if (!isCellStart(t)) {
				// 格子的属性紧跟着格子开头给，所以是给刚放进去那一个的
				if (!out.cells.empty()) {
					if (t.starts_with(" colspan=")) out.cells.back().colSpan = spanOf(t);
					else if (t.starts_with(" rowspan=")) out.cells.back().rowSpan = spanOf(t);
				}
				continue;
			}
			// 框给的是归一化坐标，乘长边就是原图像素坐标（RapidTable 两步换算的等价写法）
			Cell cell;
			const auto b = loc.data() + (size_t)i * 8;
			std::array<float, 4> xs{ b[0], b[2], b[4], b[6] };
			std::array<float, 4> ys{ b[1], b[3], b[5], b[7] };
			cell.x0 = std::min({ xs[0], xs[1], xs[2], xs[3] }) * scale;
			cell.x1 = std::max({ xs[0], xs[1], xs[2], xs[3] }) * scale;
			cell.y0 = std::min({ ys[0], ys[1], ys[2], ys[3] }) * scale;
			cell.y1 = std::max({ ys[0], ys[1], ys[2], ys[3] }) * scale;
			const auto idx = (int)out.cells.size();
			out.cells.push_back(cell);
			// 找这一行里第一个空位放：上面有 rowspan 占下来的列要先跳过
			if (row < 0) row = 0;
			while ((int)out.grid.size() <= row) out.grid.emplace_back();
			int col{ 0 };
			while (col < (int)out.grid[row].size() && out.grid[row][col] != -1) ++col;
			out.cells[idx].row = row;
			out.cells[idx].col = col;
			for (int r = row; r < row + out.cells[idx].rowSpan; ++r) {
				while ((int)out.grid.size() <= r) out.grid.emplace_back();
				for (int c = col; c < col + out.cells[idx].colSpan; ++c) {
					while ((int)out.grid[r].size() <= c) out.grid[r].push_back(-1);
					out.grid[r][c] = idx;
				}
			}
		}
		return out;
	}

	// 中文之间不加空格（加了就散了），英文之间不加就连成一团
	bool needSpace(const wchar_t a, const wchar_t b)
	{
		auto isCjk = [](const wchar_t c) { return c >= 0x2E80 && c <= 0x9FFF; };
		return !isCjk(a) && !isCjk(b);
	}

	// 整张图认一次，再把词按中心点落进各自的格子
	void fillText(std::vector<Cell>& cells, const int w, const int h, BYTE* data,
		const std::wstring& langTag)
	{
		auto words = Ocr::recognizeWords(w, h, data, langTag);
		std::vector<std::vector<int>> hits(cells.size());
		for (int i = 0; i < (int)words.size(); ++i) {
			const auto cx = words[i].x + words[i].w / 2.f;
			const auto cy = words[i].y + words[i].h / 2.f;
			for (int k = 0; k < (int)cells.size(); ++k) {
				const auto& c = cells[k];
				if (cx >= c.x0 && cx <= c.x1 && cy >= c.y0 && cy <= c.y1) {
					hits[k].push_back(i);
					break;
				}
			}
		}
		for (int k = 0; k < (int)cells.size(); ++k) {
			// 同一格里按行、再按列排，读出来才跟看到的一样
			std::sort(hits[k].begin(), hits[k].end(), [&](const int a, const int b) {
				const auto& A = words[a];
				const auto& B = words[b];
				// 差得没到一个字高的一半，当成同一行，按左右排
				if (std::fabs(A.y - B.y) > std::min(A.h, B.h) * 0.5f) return A.y < B.y;
				return A.x < B.x;
			});
			std::wstring text;
			for (const auto i : hits[k]) {
				if (!text.empty() && !words[i].text.empty()
					&& needSpace(text.back(), words[i].text.front())) {
					text += L' ';
				}
				text += words[i].text;
			}
			cells[k].text = text;
		}
	}
}

std::wstring TableResult::toHtml() const
{
	auto escape = [](const std::wstring& s) {
		std::wstring out;
		for (const auto c : s) {
			if (c == L'&') out += L"&amp;";
			else if (c == L'<') out += L"&lt;";
			else if (c == L'>') out += L"&gt;";
			else out += c;
		}
		return out;
	};
	std::wstring html{ L"<table>" };
	for (const auto& row : rows) {
		html += L"<tr>";
		for (const auto& cell : row) html += L"<td>" + escape(cell) + L"</td>";
		html += L"</tr>";
	}
	html += L"</table>";
	return html;
}

std::wstring TableResult::toTsv() const
{
	std::wstring out;
	for (const auto& row : rows) {
		if (!out.empty()) out += L"\r\n";
		for (int c = 0; c < (int)row.size(); ++c) {
			if (c > 0) out += L'\t';
			out += row[c];
		}
	}
	return out;
}

bool Table::recognize(const int w, const int h, BYTE* data,
	const std::wstring& langTag, TableResult& out, std::wstring& err)
{
	if (w <= 0 || h <= 0 || !data) return false;
	std::lock_guard lock{ mtx };
	if (!ensureSession(err)) return false;
	std::vector<float> input;
	letterbox(w, h, data, input);
	std::vector<float> loc, prob;
	int steps{ 0 };
	if (!infer(input, loc, prob, steps, err)) return false;
	auto decoded = decode(loc, prob, steps, w, h);
	if (decoded.cells.empty()) {
		err = Lang::get(L"ocr.tableEmpty");
		return false;
	}
	fillText(decoded.cells, w, h, data, langTag);
	// 摊平成规整矩阵：合并单元格只在它左上角那一格带字，跨过去的位置留空
	out.cols = 0;
	out.rows.clear();
	for (const auto& line : decoded.grid) out.cols = std::max(out.cols, (int)line.size());
	for (const auto& line : decoded.grid) {
		const auto r = (int)out.rows.size();
		std::vector<std::wstring> row;
		for (int c = 0; c < out.cols; ++c) {
			const auto idx = c < (int)line.size() ? line[c] : -1;
			if (idx < 0) {
				row.emplace_back();
				continue;
			}
			const auto& cell = decoded.cells[idx];
			row.push_back(cell.row == r && cell.col == c ? cell.text : std::wstring{});
		}
		out.rows.push_back(std::move(row));
	}
	return true;
}
