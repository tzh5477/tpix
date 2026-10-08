# -*- coding: utf-8 -*-
"""往 Src/Res/iconfont.ttf 里补线性图标。

已补进来的：时钟 / 剪贴板 / 历史 / 标尺 / 十字 / 聚光 / 设置 / 两个「依次贴」，
以及箭头工具那两种样式的图标（首尾等粗的普通箭头、尖尾渐变箭头）。

设计基准取自现有字形：em=1024，主体大致落在 x[96,928]、y[-24,880]，线宽 60~72，
视觉中心 (512, 392)。原有的字形原样保留，只往 glyf / hmtx / cmap 里加。

方向约定：外轮廓一律顺时针，孔洞一律逆时针 —— 非零环绕规则下才会真的空出来。
所有会互相重叠的形状必须同向，否则重叠处会被挖掉。

脚本可以反复跑：ICONS 里已经存在的码位 / 字形名会跳过，只补缺的那些。
"""
import math
import shutil
from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen

SRC = r'F:\personal\project\github\tpix\Src\Res\iconfont.ttf'
EM = 1024
# 45° 弧用一段二次贝塞尔近似时，控制点半径要放大 1/cos(22.5°)
K45 = 1.082392200292394


def _p(cx, cy, r, deg):
    a = math.radians(deg)
    return (cx + r * math.cos(a), cy + r * math.sin(a))


def circle(pen, cx, cy, r, cw=True):
    """整圆，8 段二次贝塞尔。cw=True 顺时针（外轮廓），False 逆时针（孔洞）。"""
    steps = 8
    sign = -1.0 if cw else 1.0
    pen.moveTo(_p(cx, cy, r, 0))
    for i in range(steps):
        a0 = sign * i * 360.0 / steps
        a1 = sign * (i + 1) * 360.0 / steps
        pen.qCurveTo(_p(cx, cy, r * K45, a0 + sign * 180.0 / steps), _p(cx, cy, r, a1))
    pen.closePath()


def ring(pen, cx, cy, r, w):
    circle(pen, cx, cy, r + w / 2.0, True)
    circle(pen, cx, cy, r - w / 2.0, False)


def disc(pen, cx, cy, r):
    circle(pen, cx, cy, r, True)


def seg(pen, x0, y0, x1, y1, w):
    """一段有宽度的线段（画成矩形，顺时针）。"""
    dx, dy = x1 - x0, y1 - y0
    d = math.hypot(dx, dy)
    if d == 0:
        return
    nx, ny = -dy / d * w / 2.0, dx / d * w / 2.0
    pen.moveTo((x0 + nx, y0 + ny))
    pen.lineTo((x1 + nx, y1 + ny))
    pen.lineTo((x1 - nx, y1 - ny))
    pen.lineTo((x0 - nx, y0 - ny))
    pen.closePath()


def rrect(pen, x0, y0, x1, y1, r, cw=True):
    """圆角矩形。圆角用单段二次贝塞尔，控制点取直角点（误差 2.7%，em 尺度下看不见）。"""
    r = max(0.0, min(r, (x1 - x0) / 2.0, (y1 - y0) / 2.0))
    if cw:
        pen.moveTo((x0 + r, y0))
        pen.qCurveTo((x0, y0), (x0, y0 + r))
        pen.lineTo((x0, y1 - r))
        pen.qCurveTo((x0, y1), (x0 + r, y1))
        pen.lineTo((x1 - r, y1))
        pen.qCurveTo((x1, y1), (x1, y1 - r))
        pen.lineTo((x1, y0 + r))
        pen.qCurveTo((x1, y0), (x1 - r, y0))
        pen.closePath()
        return
    pen.moveTo((x0, y0 + r))
    pen.qCurveTo((x0, y0), (x0 + r, y0))
    pen.lineTo((x1 - r, y0))
    pen.qCurveTo((x1, y0), (x1, y0 + r))
    pen.lineTo((x1, y1 - r))
    pen.qCurveTo((x1, y1), (x1 - r, y1))
    pen.lineTo((x0 + r, y1))
    pen.qCurveTo((x0, y1), (x0, y1 - r))
    pen.closePath()


def rrect_ring(pen, x0, y0, x1, y1, w, r):
    rrect(pen, x0, y0, x1, y1, r, True)
    rrect(pen, x0 + w, y0 + w, x1 - w, y1 - w, max(0.0, r - w), False)


# ——— 各个图标 ———————————————————————————————————————————————

CX, CY = 512, 392


def icon_clock(pen):
    w = 72
    ring(pen, CX, CY, 380, w)
    seg(pen, CX, CY, CX, CY + 280, 60)        # 分针朝上
    seg(pen, CX, CY, CX + 200, CY, 60)        # 时针朝右


def icon_clipboard(pen):
    w = 60
    rrect_ring(pen, 200, -64, 824, 800, w, 76)
    rrect(pen, 384, 712, 640, 876, 44, True)  # 压住板身上沿的夹子


def icon_history(pen):
    """两层错位的卡片 = 一叠历史。比再画个表盘更容易和时钟分开。"""
    w = 60
    rrect_ring(pen, 288, -32, 944, 616, w, 68)
    rrect_ring(pen, 80, 152, 736, 800, w, 68)


def icon_ruler(pen):
    w = 60
    rrect_ring(pen, 96, 168, 928, 552, w, 44)
    top = 552 - w
    for i, x in enumerate(range(224, 929, 128)):
        seg(pen, x, top, x, top - (108 if i % 2 else 64), 44)


def icon_crosshair(pen):
    w = 64
    seg(pen, CX, 80, CX, 704, w)
    seg(pen, 96, CY, 928, CY, w)
    ring(pen, CX, CY, 136, 56)


def icon_focus(pen):
    w, arm = 64, 240
    x0, x1, y0, y1 = 96, 928, -24, 792
    for sx, sy in ((x0, y1), (x1, y1), (x0, y0), (x1, y0)):
        hx = arm if sx == x0 else -arm
        vy = -arm if sy == y1 else arm
        seg(pen, sx, sy + vy, sx, sy, w)      # 竖臂
        seg(pen, sx, sy, sx + hx, sy, w)      # 横臂
    disc(pen, CX, 384, 168)


def icon_settings(pen):
    """三条滑轨 + 滑块。齿轮要画好得拼一堆齿，这个又清楚又不容易和别的图标撞。"""
    w = 62
    for y in (152, 392, 632):
        seg(pen, 96, y, 928, y, w)
    disc(pen, 664, 152, 112)
    disc(pen, 336, 392, 112)
    disc(pen, 616, 632, 112)


def _stack_arrow(pen):
    """右下角那根向下箭头，两个「依次贴」的图标共用。"""
    # 尖朝下：这两个图标的意思是"依次往下叠"，朝上会被读成导出
    seg(pen, 790, 880, 790, 440, 56)
    seg(pen, 656, 580, 790, 420, 56)
    seg(pen, 924, 580, 790, 420, 56)


def icon_pin_older(pen):
    """一张卡片 + 下箭头 = 依次往下贴更早的历史截图。"""
    rrect_ring(pen, 96, 400, 600, 880, 56, 60)
    _stack_arrow(pen)


def icon_pin_clip_older(pen):
    """同上，主体换成剪贴板。"""
    rrect_ring(pen, 96, 344, 600, 800, 56, 60)
    rrect(pen, 272, 764, 424, 888, 34, True)
    _stack_arrow(pen)


def icon_arrow_plain(pen):
    """首尾等粗的普通箭头：平口尾 + 等宽箭杆 + 三角头。"""
    x_tail, x_neck, x_tip = 96, 640, 928
    shaft, head = 40, 200
    pen.moveTo((x_tail, CY + shaft))
    pen.lineTo((x_neck, CY + shaft))
    pen.lineTo((x_neck, CY + head))
    pen.lineTo((x_tip, CY))
    pen.lineTo((x_neck, CY - head))
    pen.lineTo((x_neck, CY - shaft))
    pen.lineTo((x_tail, CY - shaft))
    pen.closePath()


def icon_arrow_taper(pen):
    """尖尾箭头：尾巴收成一个点，一路加宽到箭头。与上一个的区别只在尾部。"""
    x_tail, x_neck, x_tip = 96, 640, 928
    shaft, head = 40, 200
    pen.moveTo((x_tail, CY))
    pen.lineTo((x_neck, CY + shaft))
    pen.lineTo((x_neck, CY + head))
    pen.lineTo((x_tip, CY))
    pen.lineTo((x_neck, CY - head))
    pen.lineTo((x_neck, CY - shaft))
    pen.closePath()


def icon_copy(pen):
    """两张错位的页 = 复制。前页整圈描边；后页只描没被前页压住的那几段
    （整圈会在前页的肚子里串线，13px 下糊成一团），画法同 Feather 的 copy。"""
    w = 56
    rrect_ring(pen, 352, 32, 880, 560, w, 64)   # 前页（右下），完整描边
    seg(pen, 144, 424, 144, 848, w)             # 后页左边（沿用到上边）
    seg(pen, 144, 848, 672, 848, w)             # 后页上边
    seg(pen, 672, 848, 672, 656, w)             # 后页右边一小段，末端悬空（离前页上沿留个缺口）


# ——— 悬浮球上换掉 / 新增的那几个 ———————————————————————————————
# 换过的原因都写在 BallAction.cpp 的表旁边：要的是"一眼分得清谁是谁"，
# 17 像素下轮廓比细节重要，所以每个都只留一两块大形状。

def icon_image(pen):
    """相框 + 山 + 日头 = 一张图片。
    原来的「从图片文件贴图」用的是"箭头落进托盘"，那画的是**保存**，意思正好反了。"""
    w = 62
    rrect_ring(pen, 96, 168, 928, 700, w, 64)
    # 山：三角峰，底边正好压在内框的下沿上
    pen.moveTo((470, 480))
    pen.lineTo((750, 230))
    pen.lineTo((210, 230))
    pen.closePath()
    disc(pen, 296, 570, 60)


def icon_image_older(pen):
    """相框（图）+ 右下角下箭头 = 依次往下贴更早的截图。
    与上面那枚只差一个箭头 —— 单张 vs 一叠，靠箭头分。"""
    w = 56
    rrect_ring(pen, 96, 400, 600, 880, w, 60)
    pen.moveTo((355, 700))
    pen.lineTo((500, 456))
    pen.lineTo((190, 456))
    pen.closePath()
    disc(pen, 238, 756, 44)
    _stack_arrow(pen)


def icon_clip_older(pen):
    """剪贴板（夹子加宽、上移一点，小尺寸下也认得出）+ 右下角同一个下箭头。
    原来这一枚与 icon_pin_older 都是"一张空心卡片 + 同一根箭头"，缩到 17 像素分不出来。"""
    w = 56
    rrect_ring(pen, 96, 344, 600, 800, w, 60)
    rrect(pen, 256, 760, 440, 888, 36, True)
    _stack_arrow(pen)


def icon_history_arrow(pen):
    """左向箭头 + 表盘 = 往前翻记录。
    原来的版本是两张错位卡片，和"复制"（icon_copy）几乎一模一样。"""
    w = 58
    # 左向箭头（箭杆 + 三角头，两者都是顺时针，重叠处取并集）
    seg(pen, 140, 760, 620, 760, 56)
    pen.moveTo((130, 760))
    pen.lineTo((300, 865))
    pen.lineTo((300, 655))
    pen.closePath()
    # 表盘：一圈 + 两根指针
    ring(pen, 600, 300, 230, w)
    seg(pen, 600, 300, 600, 430, 50)
    seg(pen, 600, 300, 500, 300, 50)


def icon_translate(pen):
    """地球（一圈 + 赤道 + 竖直的经线梭）= 翻译。整个字形里就它有"圈里带经纬"的样子。"""
    w = 62
    r = 370
    ring(pen, CX, CY, r, w)
    seg(pen, CX - r, CY, CX + r, CY, 58)            # 赤道
    # 经线：上下收尖的梭形，尖端落在环的中线上（看着就是长在环上）
    pen.moveTo((CX, CY + r))
    pen.qCurveTo((CX + 250, CY), (CX, CY - r))
    pen.qCurveTo((CX - 250, CY), (CX, CY + r))
    pen.closePath()


def icon_chat(pen):
    """对话气泡（圆角框 + 左下角的尾巴）+ 两行字 = AI 对话"""
    w = 58
    rrect_ring(pen, 96, 232, 928, 800, w, 96)
    # 尾巴：三角，上沿压在气泡下边框里，下半截探出去
    pen.moveTo((240, 240))
    pen.lineTo((430, 240))
    pen.lineTo((240, 40))
    pen.closePath()
    for y in (600, 420):
        seg(pen, 240, y, 784, y, 60)


ICONS = [
    (0xE909, 'arrowPlain', icon_arrow_plain),
    (0xE90A, 'arrowTaper', icon_arrow_taper),
    (0xE90B, 'copy', icon_copy),
    (0xE907, 'pinOlder', icon_pin_older),
    (0xE908, 'pinClipOlder', icon_pin_clip_older),
    (0xE900, 'clock', icon_clock),
    (0xE901, 'clipboard', icon_clipboard),
    (0xE902, 'history', icon_history),
    (0xE903, 'ruler', icon_ruler),
    (0xE904, 'crosshair', icon_crosshair),
    (0xE905, 'focus', icon_focus),
    (0xE906, 'settings', icon_settings),
    # 上面 0xE907 / 0xE908 / 0xE902 三个旧字形留在字体里不动（脚本跳过已存在的码位），
    # 悬浮球改用了下面这几枚新形状
    (0xE90C, 'image', icon_image),
    (0xE90D, 'historyArrow', icon_history_arrow),
    (0xE90E, 'imageOlder', icon_image_older),
    (0xE90F, 'clipOlder', icon_clip_older),
    (0xE910, 'translate', icon_translate),
    (0xE911, 'chat', icon_chat),
]


def main():
    shutil.copy2(SRC, SRC + '.bak')
    f = TTFont(SRC)
    order = f.getGlyphOrder()
    cmap = f.getBestCmap()
    added = 0
    for cp, name, fn in ICONS:
        # 已经补过的跳过：脚本反复跑不该把同一个码位写两遍，更不该把旧字形覆盖掉
        if name in order or cp in cmap:
            continue
        pen = TTGlyphPen(None)
        fn(pen)
        glyph = pen.glyph()
        glyph.recalcBounds(f['glyf'])
        order.append(name)
        f['glyf'][name] = glyph
        f['hmtx'][name] = (EM, glyph.xMin)
        for sub in f['cmap'].tables:
            if sub.isUnicode():
                sub.cmap[cp] = name
        added += 1
    if added == 0:
        print('没有要补的字形，字体未改动')
        return
    f.setGlyphOrder(order)
    f['maxp'].numGlyphs = len(order)
    f.save(SRC)
    print('新增', added, '个字形，总数', len(order))


main()
