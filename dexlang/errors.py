"""工具链统一的错误类型。"""


class DexError(Exception):
    def __init__(self, message, line=None, col=None, phase=None):
        self.message = message
        self.line = line
        self.col = col
        self.phase = phase
        super().__init__(self._format())

    def _format(self):
        loc = ""
        if self.line is not None:
            loc = f" at {self.line}:{self.col or 1}"
        prefix = f"[{self.phase}]" if self.phase else ""
        return f"{prefix}error{loc}: {self.message}"
