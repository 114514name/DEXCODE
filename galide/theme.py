"""galide.theme — 编辑器配色主题。"""

BG = "#1e1f26"            # 主背景
BG2 = "#262832"           # 面板背景
BG3 = "#2f3340"           # 输入/控件背景
FG = "#e8eaf0"            # 主文字
FG2 = "#a0a6b5"           # 次要文字
ACCENT = "#4f8cff"        # 高亮/选中
ACCENT_DARK = "#3a6fd0"
BORDER = "#3a3e4c"
OK = "#3ddc84"
WARN = "#f0b90b"

FONT = ("Microsoft YaHei UI", 10)
FONT_SM = ("Microsoft YaHei UI", 9)
FONT_BOLD = ("Microsoft YaHei UI", 10, "bold")
MONO = ("Consolas", 10)

# 块类型 → (填充, 边框, 前景)
BLOCK_COLORS = {
    0: ("#2d4a7a", "#4f8cff", "#ffffff"),   # SPEAK 说话 蓝
    1: ("#4a3a7a", "#8f6bff", "#ffffff"),   # BG 背景 紫
    2: ("#1f6a5c", "#35c6a6", "#ffffff"),   # SPRITE 立绘 青
    3: ("#7a4a1f", "#ff9a3d", "#ffffff"),   # AUDIO 音频 橙
    4: ("#6a631f", "#e0c53a", "#ffffff"),   # WAIT 等待 黄
    5: ("#3a3f4a", "#8b93a3", "#ffffff"),   # CLEAR 清空 灰
    6: ("#6a2436", "#ff5a6e", "#ffffff"),   # CHOICE 选项 红
    7: ("#5a2a4a", "#e87fb4", "#ffffff"),   # OPTION 选项项 粉
    8: ("#1f4a2a", "#3ddc84", "#ffffff"),   # IF 如果 绿
    9: ("#1f3a5a", "#35a7ff", "#ffffff"),   # CODE 代码 深蓝
    10: ("#2a2d36", "#6a7080", "#aab2c0"),  # END 结束 深灰
}

BLOCK_ICONS = {
    0: "💬", 1: "🖼", 2: "👤", 3: "🎵", 4: "⏳",
    5: "🗑", 6: "❓", 7: "➤", 8: "🔀", 9: "⌨", 10: "⏹",
}
