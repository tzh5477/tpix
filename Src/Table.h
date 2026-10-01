#pragma once
#include <string>
#include <vector>

// 表格识别的结果。格子摊平成规整矩阵：合并单元格按占位展开，文字只在它左上角那一格，
// 跨过去的行列留空 —— HTML 与 TSV 都基于这个矩阵，粘到 Excel 里行列对得齐
struct TableResult
{
	int cols{ 0 };
	std::vector<std::vector<std::wstring>> rows;
	// 拼成 <table>…</table>，格子里的内容做过转义。给 Word / Excel 这类认 HTML 的程序用
	std::wstring toHtml() const;
	// 行内 tab 分隔、行间换行。给记事本这类只认纯文本的程序用，贴进 Excel 也是一张表
	std::wstring toTsv() const;
};

// 表格识别。两步：结构用 slanet-plus（RapidTable 的模型，ONNX，首次使用时下到数据目录），
// 格子里的字用系统 OCR 填。整张图只过一次模型、只认一次字 —— 逐格裁开去认要慢上几十倍。
class Table
{
public:
	// 识别一张 BGRA top-down 的图。成了返回 true；失败时 err 是一句能直接给用户看的话
	//（模型没下下来、图里没找到表格…）。要跑模型、还可能要先下载，几秒才回得来，
	// 别在 UI 线程上直接调
	static bool recognize(const int w, const int h, BYTE* data,
		const std::wstring& langTag, TableResult& out, std::wstring& err);
};
