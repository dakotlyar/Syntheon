"""
SynBench GUI – PySide6 dark-themed application.
"""

import platform
import subprocess
import sys
import threading
from typing import Dict, Optional

from PySide6.QtCore import Qt, QThread, Signal, QTimer, QRectF
from PySide6.QtGui import QPainter, QPainterPath, QColor, QPen, QFont, QBrush
from PySide6.QtWidgets import (
    QApplication, QDialog, QFileDialog, QFrame, QHBoxLayout, QLabel,
    QLineEdit, QMainWindow, QMessageBox, QPushButton, QCheckBox,
    QScrollArea, QSizePolicy, QStatusBar, QTextEdit, QVBoxLayout, QWidget,
)

# ---------------------------------------------------------------------------
# Colour palette
# ---------------------------------------------------------------------------
C = {
    "bg":       "#0d1117",
    "surface":  "#161b22",
    "surface2": "#21262d",
    "border":   "#30363d",
    "text":     "#e6edf3",
    "muted":    "#8b949e",
    "accent":   "#238636",
    "accent_h": "#2ea043",
    "blue":     "#1f6feb",
    "blue_h":   "#388bfd",
    "error":    "#f85149",
    "cpu":      "#58a6ff",
    "gpu":      "#3fb950",
    "npu":      "#a371f7",
    "ram":      "#d2a8ff",
    "disk":     "#ffa657",
    "overall":  "#f0c040",
}

MAX_SCORES  = {"cpu": 3000, "gpu": 2000, "npu": 200, "ram": 200, "disk": 200}
OVERALL_MAX = sum(MAX_SCORES.values())
TESTS       = ["cpu", "gpu", "npu", "ram", "disk"]
TEST_LABELS = {"cpu": "CPU", "gpu": "GPU", "npu": "NPU", "ram": "RAM", "disk": "Disk"}

GLOBAL_STYLE = f"""
QWidget       {{ background-color:{C["bg"]}; color:{C["text"]}; font-size:11px; }}
QMainWindow   {{ background-color:{C["bg"]}; }}
QDialog       {{ background-color:{C["bg"]}; }}
QMenuBar      {{ background-color:{C["surface"]}; border-bottom:1px solid {C["border"]}; }}
QMenuBar::item:selected  {{ background:{C["blue"]}; }}
QMenu         {{ background:{C["surface"]}; border:1px solid {C["border"]}; }}
QMenu::item:selected     {{ background:{C["blue"]}; }}
QScrollArea   {{ border:none; }}
QScrollBar:vertical      {{ background:{C["bg"]}; width:8px; border:none; }}
QScrollBar::handle:vertical {{ background:{C["border"]}; border-radius:4px; min-height:20px; }}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{ height:0; }}
QStatusBar    {{ background:{C["surface2"]}; color:{C["muted"]}; border-top:1px solid {C["border"]}; font-size:10px; }}
"""


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def _mk_btn(text, bg, hover, fg="white", bold=True, fs=10, px=16, py=8) -> QPushButton:
    b = QPushButton(text)
    w = "bold" if bold else "normal"
    b.setStyleSheet(f"""
        QPushButton {{
            background:{bg}; color:{fg}; border:none; border-radius:4px;
            padding:{py}px {px}px; font-size:{fs}px; font-weight:{w};
        }}
        QPushButton:hover   {{ background:{hover}; }}
        QPushButton:disabled {{ background:{C["surface2"]}; color:{C["muted"]}; }}
    """)
    return b


def _mk_lbl(text, color=None, fs=None, bold=False) -> QLabel:
    lbl = QLabel(text)
    parts = ["background:transparent;"]
    if color: parts.append(f"color:{color};")
    if fs:    parts.append(f"font-size:{fs}px;")
    if bold:  parts.append("font-weight:bold;")
    lbl.setStyleSheet(" ".join(parts))
    return lbl


# ---------------------------------------------------------------------------
# System-info helpers (unchanged from tkinter version)
# ---------------------------------------------------------------------------

def _get_cpu_brand() -> str:
    try:
        if platform.system() == "Linux":
            with open("/proc/cpuinfo") as f:
                for line in f:
                    if line.startswith("model name"):
                        return line.split(":")[1].strip()
        elif platform.system() == "Darwin":
            out = subprocess.check_output(["sysctl", "-n", "machdep.cpu.brand_string"])
            return out.decode().strip()
        elif platform.system() == "Windows":
            import winreg
            key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                 r"HARDWARE\DESCRIPTION\System\CentralProcessor\0")
            return winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    except Exception:
        pass
    return platform.processor() or "Unknown"


def _get_cpu_name() -> str:
    try:
        import psutil
        freq   = psutil.cpu_freq()
        cores  = psutil.cpu_count(logical=False)
        threads = psutil.cpu_count(logical=True)
        freq_s = f" @ {freq.max/1000:.2f} GHz" if freq else ""
        return f"{cores}C/{threads}T{freq_s}"
    except Exception:
        return platform.processor() or "Unknown"


def _get_ram_info() -> str:
    try:
        import psutil
        gb = psutil.virtual_memory().total / (1024 ** 3)
        return f"{gb:.1f} GB"
    except Exception:
        return "Unknown"


def _get_gpu_name() -> str:
    try:
        out = subprocess.check_output(
            ["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"], timeout=3)
        names = [l.strip() for l in out.decode().splitlines() if l.strip()]
        if names:
            return ", ".join(names)
    except Exception:
        pass
    try:
        out = subprocess.check_output(["rocm-smi", "--showproductname"], timeout=3)
        for line in out.decode().splitlines():
            if "Card series:" in line or "GPU[" in line:
                return line.strip()
    except Exception:
        pass
    return "Not detected"


def _get_os_info() -> str:
    if platform.system() == "Linux":
        try:
            with open("/etc/os-release") as f:
                info = {}
                for line in f:
                    if "=" in line:
                        k, v = line.strip().split("=", 1)
                        info[k] = v.strip('"')
            return info.get("PRETTY_NAME", platform.platform())
        except Exception:
            pass
    return platform.platform()


# ---------------------------------------------------------------------------
# ScoreGauge – semicircular arc gauge
# ---------------------------------------------------------------------------

class ScoreGauge(QWidget):
    def __init__(self, size: int = 200, color: str = C["overall"], parent=None):
        super().__init__(parent)
        self._sz    = size
        self._color = QColor(color)
        self._score = 0
        self._max   = 100
        self._label = ""
        self.setFixedSize(size, size // 2 + 40)
        self.setAttribute(Qt.WA_OpaquePaintEvent, False)

    def set(self, score: int, maximum: int, label: str = ""):
        self._score = score
        self._max   = maximum
        self._label = label
        self.update()

    def paintEvent(self, _event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)

        s  = self._sz
        cx = s // 2
        cy = s // 2
        r  = s // 2 - 8

        rect = QRectF(cx - r, cy - r, 2 * r, 2 * r)

        # Background arc (upper semicircle)
        pen_bg = QPen(QColor(C["border"]), 18, Qt.SolidLine, Qt.FlatCap)
        p.setPen(pen_bg)
        p.drawArc(rect, 0 * 16, 180 * 16)

        # Filled arc – grows from left to right
        frac = min(1.0, max(0.0, self._score / max(self._max, 1)))
        if frac > 0:
            start = int((180 - frac * 180) * 16)
            span  = int(frac * 180 * 16)
            pen_fg = QPen(self._color, 18, Qt.SolidLine, Qt.FlatCap)
            p.setPen(pen_fg)
            p.drawArc(rect, start, span)

        # Score text
        p.setPen(QColor(C["text"]))
        f = QFont()
        f.setPointSize(18)
        f.setBold(True)
        p.setFont(f)
        p.drawText(QRectF(0, cy - 18, s, 36), Qt.AlignCenter, str(self._score))

        if self._label:
            p.setPen(QColor(C["muted"]))
            f2 = QFont()
            f2.setPointSize(8)
            p.setFont(f2)
            p.drawText(QRectF(0, cy + 18, s, 20), Qt.AlignCenter, self._label)

        p.end()


# ---------------------------------------------------------------------------
# Bar widget (rounded-rect progress bar)
# ---------------------------------------------------------------------------

class _BarWidget(QWidget):
    def __init__(self, color: QColor, parent=None):
        super().__init__(parent)
        self._color = color
        self._frac  = 0.0
        self._bar_color = color

    def set_fraction(self, frac: float):
        self._frac = min(1.0, max(0.0, frac))
        self._bar_color = self._color
        self.update()

    def set_progress(self, frac: float, color: QColor):
        self._frac = min(1.0, max(0.0, frac))
        self._bar_color = color
        self.update()

    def paintEvent(self, _event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        w, h = self.width(), self.height()
        r = h / 2.0

        bg = QPainterPath()
        bg.addRoundedRect(QRectF(0, 0, w, h), r, r)
        p.fillPath(bg, QColor(C["border"]))

        filled_w = self._frac * w
        if filled_w >= r * 2:
            fg = QPainterPath()
            fg.addRoundedRect(QRectF(0, 0, filled_w, h), r, r)
            p.fillPath(fg, self._bar_color)

        p.end()


# ---------------------------------------------------------------------------
# ScoreRow
# ---------------------------------------------------------------------------

class ScoreRow(QWidget):
    def __init__(self, label: str, color: str, max_score: int, parent=None):
        super().__init__(parent)
        self._max = max_score
        self.setStyleSheet(f"background-color:{C['surface2']}; border-radius:4px;")

        outer = QVBoxLayout(self)
        outer.setContentsMargins(12, 6, 12, 4)
        outer.setSpacing(2)

        row = QHBoxLayout()
        row.setSpacing(8)

        lbl = QLabel(label)
        lbl.setFixedWidth(48)
        lbl.setStyleSheet(f"color:{color}; font-weight:bold; font-size:11px; background:transparent;")
        row.addWidget(lbl)

        self._bar = _BarWidget(QColor(color))
        self._bar.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self._bar.setFixedHeight(16)
        row.addWidget(self._bar)

        self._score_lbl = QLabel("—")
        self._score_lbl.setFixedWidth(85)
        self._score_lbl.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
        self._score_lbl.setStyleSheet(
            f"color:{C['text']}; font-weight:bold; font-size:11px; background:transparent;")
        row.addWidget(self._score_lbl)
        outer.addLayout(row)

        self._detail_lbl = QLabel("")
        self._detail_lbl.setStyleSheet(
            f"color:{C['muted']}; font-size:8px; background:transparent;")
        outer.addWidget(self._detail_lbl)

    def set_score(self, score: int, detail: str = "", unavailable: bool = False):
        self._bar.set_fraction(score / max(self._max, 1))
        if unavailable:
            self._score_lbl.setText("N/A")
            self._score_lbl.setStyleSheet(
                f"color:{C['muted']}; font-weight:bold; font-size:11px; background:transparent;")
        else:
            self._score_lbl.setText(f"{score} pts")
            self._score_lbl.setStyleSheet(
                f"color:{C['text']}; font-weight:bold; font-size:11px; background:transparent;")
        self._detail_lbl.setText(detail)

    def set_progress(self, fraction: float, text: str = ""):
        self._bar.set_progress(fraction, QColor(C["blue"]))
        self._detail_lbl.setText(text)


# ---------------------------------------------------------------------------
# Benchmark worker thread
# ---------------------------------------------------------------------------

class BenchmarkWorker(QThread):
    sig_progress = Signal(str, str, float)   # test, message, done_fraction
    sig_result   = Signal(str, object)        # test, BenchmarkResult
    sig_status   = Signal(str)
    sig_done     = Signal(dict)
    sig_error    = Signal(str)

    def __init__(self, selected, disk_dir: str):
        super().__init__()
        self._selected = selected
        self._disk_dir = disk_dir
        self._running  = True

    def stop(self):
        self._running = False

    def run(self):
        try:
            self._run_benchmarks()
        except Exception as exc:
            import traceback
            self.sig_error.emit(
                f"Benchmark worker crashed:\n\n{exc}\n\n{traceback.format_exc()}")

    def _run_benchmarks(self):
        n = len(self._selected)
        done = [0]

        from benchmarks.cpu  import CPUBenchmark
        from benchmarks.gpu  import GPUBenchmark
        from benchmarks.npu  import NPUBenchmark
        from benchmarks.ram  import RAMBenchmark
        from benchmarks.disk import DiskBenchmark
        from benchmarks import BenchmarkResult

        registry = {
            "cpu": CPUBenchmark, "ram": RAMBenchmark, "gpu": GPUBenchmark,
            "disk": DiskBenchmark, "npu": NPUBenchmark,
        }

        results = {}
        for test in self._selected:
            if not self._running:
                break
            self.sig_status.emit(f"Running {TEST_LABELS[test]} benchmark …")

            def cb(msg, t=test):
                if not self._running:
                    raise InterruptedError()
                self.sig_progress.emit(t, msg, done[0] / n)

            try:
                kw = {"test_dir": self._disk_dir} if test == "disk" else {}
                result = registry[test](callback=cb, **kw).run()
            except InterruptedError:
                break
            except Exception as exc:
                result = BenchmarkResult(
                    name=test, score=0, available=False,
                    summary="Error", error=str(exc))

            results[test] = result
            done[0] += 1
            self.sig_result.emit(test, result)

        self.sig_done.emit(results)


# ---------------------------------------------------------------------------
# Runtime setup dialog
# ---------------------------------------------------------------------------

class RuntimeSetupDialog(QDialog):
    _log_sig = Signal(str)

    def __init__(self, parent, recommendations):
        super().__init__(parent)
        self._recs = recommendations
        self._btns: Dict[str, QPushButton] = {}
        self.setWindowTitle("Optional Runtime Setup")
        self.resize(580, 500)
        self.setMinimumSize(480, 380)
        self._log_sig.connect(self._do_log)
        self._build_ui()

    def _build_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        # Header
        hdr = QWidget()
        hdr.setStyleSheet(f"background-color:{C['surface']};")
        hl = QVBoxLayout(hdr)
        hl.setContentsMargins(16, 14, 16, 14)
        hl.addWidget(_mk_lbl("Optional Runtime Setup", C["text"], fs=15, bold=True))
        hl.addWidget(_mk_lbl(
            "Hardware detected that could benefit from additional packages.",
            C["muted"], fs=9))
        layout.addWidget(hdr)

        sep = QFrame(); sep.setFrameShape(QFrame.HLine)
        sep.setStyleSheet(f"color:{C['border']};")
        layout.addWidget(sep)

        # Recommendation cards
        for rec in self._recs:
            color = C["gpu"] if rec.category == "gpu" else C["npu"]
            card = QWidget()
            card.setStyleSheet(
                f"background-color:{C['surface2']}; border-radius:4px;")
            cl = QHBoxLayout(card)
            cl.setContentsMargins(10, 10, 10, 10)
            cl.setSpacing(10)

            tag = QLabel(rec.category.upper())
            tag.setFixedSize(38, 22)
            tag.setAlignment(Qt.AlignCenter)
            tag.setStyleSheet(
                f"background:{color}; color:white; font-size:8px; "
                f"font-weight:bold; border-radius:3px;")
            cl.addWidget(tag)

            info = QVBoxLayout()
            info.setSpacing(2)
            info.addWidget(_mk_lbl(rec.label, C["text"], fs=11, bold=True))
            info.addWidget(_mk_lbl(rec.reason, C["muted"], fs=8))
            info.addWidget(_mk_lbl(f"pip install {rec.pip_name}",
                                   C["muted"], fs=8))
            cl.addLayout(info, 1)

            btn = _mk_btn("Install", C["accent"], C["accent_h"], fs=9, px=12, py=6)
            btn.clicked.connect(lambda checked=False, r=rec: self._install(r))
            cl.addWidget(btn)
            self._btns[rec.pip_name] = btn

            wrapper = QVBoxLayout()
            wrapper.setContentsMargins(16, 6, 16, 0)
            wrapper.addWidget(card)
            w = QWidget(); w.setLayout(wrapper)
            layout.addWidget(w)

        # Log
        log_hdr = QWidget()
        log_hdr.setContentsMargins(0, 0, 0, 0)
        lhl = QVBoxLayout(log_hdr)
        lhl.setContentsMargins(16, 10, 16, 2)
        lhl.addWidget(_mk_lbl("INSTALL LOG", C["muted"], fs=9, bold=True))
        layout.addWidget(log_hdr)

        log_wrap = QWidget()
        log_wrap.setStyleSheet(f"background:{C['surface']}; border-radius:4px;")
        lwl = QVBoxLayout(log_wrap)
        lwl.setContentsMargins(8, 8, 8, 8)
        self._log = QTextEdit()
        self._log.setReadOnly(True)
        self._log.setStyleSheet(
            f"background:{C['surface']}; color:{C['muted']}; "
            f"border:none; font-family:monospace; font-size:8px;")
        lwl.addWidget(self._log)
        log_outer = QVBoxLayout(); log_outer.setContentsMargins(16, 0, 16, 0)
        log_outer.addWidget(log_wrap)
        lw = QWidget(); lw.setLayout(log_outer)
        layout.addWidget(lw, 1)

        # Footer
        sep2 = QFrame(); sep2.setFrameShape(QFrame.HLine)
        sep2.setStyleSheet(f"color:{C['border']};")
        layout.addWidget(sep2)

        footer = QWidget()
        footer.setStyleSheet(f"background:{C['bg']};")
        fl = QHBoxLayout(footer)
        fl.setContentsMargins(16, 10, 16, 10)

        from PySide6.QtWidgets import QCheckBox as _CB
        self._skip_cb = _CB("Don't show this again")
        self._skip_cb.setStyleSheet(
            f"color:{C['muted']}; font-size:9px; background:transparent;")
        fl.addWidget(self._skip_cb)
        fl.addStretch()

        close_btn = _mk_btn("Close", C["surface2"], C["surface"],
                            C["text"], bold=False, fs=10, px=18, py=6)
        close_btn.clicked.connect(self._close)
        fl.addWidget(close_btn)
        layout.addWidget(footer)

    def _install(self, rec):
        btn = self._btns[rec.pip_name]
        btn.setText("Installing …")
        btn.setEnabled(False)
        btn.setStyleSheet(btn.styleSheet().replace(C["accent"], C["blue"]))

        def run():
            from runtime import install_package
            ok, msg = install_package(rec.pip_name, self._log_sig.emit)
            label = "✓ Installed" if ok else "✗ Failed"
            color = C["accent"] if ok else C["error"]
            self._log_sig.emit(("\n✓ " if ok else "\n✗ ") + msg)
            QTimer.singleShot(0, lambda: (
                btn.setText(label),
                btn.setStyleSheet(btn.styleSheet()
                                  .replace(C["blue"], color)),
            ))

        threading.Thread(target=run, daemon=True).start()

    def _do_log(self, text: str):
        self._log.append(text.rstrip())

    def _close(self):
        from runtime import set_runtime_setup_skipped
        set_runtime_setup_skipped(self._skip_cb.isChecked())
        self.accept()


# ---------------------------------------------------------------------------
# Main application window
# ---------------------------------------------------------------------------

class BenchmarkApp(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("SynBench – Synthetic System Benchmark")
        self.resize(820, 780)
        self.setMinimumSize(700, 600)

        self._results: Dict = {}
        self._worker: Optional[BenchmarkWorker] = None
        self._score_rows: Dict[str, ScoreRow] = {}
        self._checkboxes: Dict[str, QCheckBox] = {}

        self._build_menu()
        self._build_ui()
        self._load_system_info()
        QTimer.singleShot(600, self._check_runtimes)

    # ------------------------------------------------------------------
    # Menu
    # ------------------------------------------------------------------

    def _build_menu(self):
        mb = self.menuBar()

        file_m = mb.addMenu("File")
        file_m.addAction("Export Results (JSON) …", self._export_json)
        file_m.addAction("Export Results (Text) …", self._export_text)
        file_m.addSeparator()
        file_m.addAction("Quit", self.close)

        tools_m = mb.addMenu("Tools")
        tools_m.addAction("Install Optional Runtimes …", self._open_runtime_setup)

        help_m = mb.addMenu("Help")
        help_m.addAction("About SynBench", self._show_about)

    # ------------------------------------------------------------------
    # Build UI
    # ------------------------------------------------------------------

    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        # Header
        root.addWidget(self._mk_header())

        sep = QFrame()
        sep.setFrameShape(QFrame.HLine)
        sep.setStyleSheet(f"color:{C['border']};")
        root.addWidget(sep)

        # Scrollable body
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        body = QWidget()
        body.setStyleSheet(f"background:{C['bg']};")
        bl = QVBoxLayout(body)
        bl.setContentsMargins(16, 16, 16, 16)
        bl.setSpacing(8)
        scroll.setWidget(body)
        root.addWidget(scroll, 1)

        bl.addWidget(self._mk_selection_card())
        bl.addLayout(self._mk_button_row())
        bl.addWidget(self._mk_score_card())
        bl.addWidget(self._mk_log_card())
        bl.addStretch()

        self._status = QStatusBar()
        self._status.showMessage("Ready")
        self.setStatusBar(self._status)

    def _mk_header(self) -> QWidget:
        w = QWidget()
        w.setStyleSheet(f"background:{C['surface']};")
        layout = QHBoxLayout(w)
        layout.setContentsMargins(20, 14, 20, 14)

        col = QVBoxLayout()
        col.setSpacing(2)
        col.addWidget(_mk_lbl("SynBench", C["text"], fs=22, bold=True))
        col.addWidget(_mk_lbl("Synthetic System Benchmark", C["muted"], fs=10))
        layout.addLayout(col)
        layout.addStretch()

        info = QVBoxLayout()
        info.setSpacing(1)
        self._cpu_lbl = _mk_lbl("CPU: …", C["muted"], fs=9)
        self._ram_lbl = _mk_lbl("RAM: …", C["muted"], fs=9)
        self._gpu_lbl = _mk_lbl("GPU: …", C["muted"], fs=9)
        self._os_lbl  = _mk_lbl("OS: …",  C["muted"], fs=9)
        for lbl in (self._cpu_lbl, self._ram_lbl, self._gpu_lbl, self._os_lbl):
            lbl.setAlignment(Qt.AlignRight)
            info.addWidget(lbl)
        layout.addLayout(info)
        return w

    def _mk_selection_card(self) -> QWidget:
        card = QWidget()
        card.setStyleSheet(f"background:{C['surface']}; border-radius:6px;")
        layout = QVBoxLayout(card)
        layout.setContentsMargins(16, 12, 16, 12)
        layout.setSpacing(8)
        layout.addWidget(_mk_lbl("SELECT TESTS", C["muted"], fs=10, bold=True))

        cb_row = QHBoxLayout()
        cb_row.setSpacing(0)
        for t in TESTS:
            cb = QCheckBox(TEST_LABELS[t])
            cb.setChecked(True)
            cb.setStyleSheet(f"""
                QCheckBox {{
                    color:{C[t]}; font-weight:bold; font-size:11px;
                    spacing:5px; background:transparent;
                }}
                QCheckBox::indicator {{
                    width:14px; height:14px;
                    border:1px solid {C['border']}; border-radius:3px;
                    background:{C['surface2']};
                }}
                QCheckBox::indicator:checked {{
                    background:{C[t]}; border-color:{C[t]};
                }}
            """)
            self._checkboxes[t] = cb
            cb_row.addWidget(cb)
            cb_row.addSpacing(18)
        cb_row.addStretch()
        layout.addLayout(cb_row)

        # Disk directory
        disk_row = QHBoxLayout()
        disk_row.setSpacing(6)
        disk_row.addWidget(_mk_lbl("Disk test dir:", C["muted"], fs=9))

        from benchmarks.disk import _find_real_disk_dir
        self._disk_edit = QLineEdit(_find_real_disk_dir())
        self._disk_edit.setFixedWidth(300)
        self._disk_edit.setStyleSheet(f"""
            QLineEdit {{
                background:{C['surface2']}; color:{C['text']};
                border:1px solid {C['border']}; border-radius:3px;
                padding:2px 6px; font-size:9px;
            }}
        """)
        disk_row.addWidget(self._disk_edit)

        browse = QPushButton("Browse…")
        browse.setStyleSheet(f"""
            QPushButton {{
                background:{C['surface2']}; color:{C['muted']};
                border:none; padding:2px 8px; font-size:9px; border-radius:3px;
            }}
            QPushButton:hover {{ color:{C['text']}; }}
        """)
        browse.clicked.connect(self._browse_disk)
        disk_row.addWidget(browse)
        disk_row.addStretch()
        layout.addLayout(disk_row)
        return card

    def _mk_button_row(self) -> QHBoxLayout:
        row = QHBoxLayout()
        row.setSpacing(10)

        self._run_btn = _mk_btn("▶  Run Benchmarks", C["accent"], C["accent_h"], fs=12)
        self._run_btn.clicked.connect(self._start_benchmarks)
        row.addWidget(self._run_btn)

        self._export_btn = _mk_btn("Export Results", C["surface2"], C["surface"],
                                   C["text"], bold=False, fs=10)
        self._export_btn.setEnabled(False)
        self._export_btn.clicked.connect(self._export_json)
        row.addWidget(self._export_btn)

        row.addStretch()
        return row

    def _mk_score_card(self) -> QWidget:
        card = QWidget()
        card.setStyleSheet(f"background:{C['surface']}; border-radius:6px;")
        layout = QHBoxLayout(card)
        layout.setContentsMargins(16, 14, 16, 14)
        layout.setSpacing(20)

        gauge_col = QVBoxLayout()
        gauge_col.setSpacing(4)
        self._overall_gauge = ScoreGauge(size=200, color=C["overall"])
        gauge_col.addWidget(self._overall_gauge, 0, Qt.AlignHCenter)
        gauge_col.addWidget(
            _mk_lbl("OVERALL SCORE", C["overall"], fs=9, bold=True))
        layout.addLayout(gauge_col)

        right = QVBoxLayout()
        right.setSpacing(4)
        right.addWidget(_mk_lbl("Results", C["text"], fs=13, bold=True))
        for t in TESTS:
            row = ScoreRow(TEST_LABELS[t], C[t], MAX_SCORES[t])
            right.addWidget(row)
            self._score_rows[t] = row
        layout.addLayout(right, 1)
        return card

    def _mk_log_card(self) -> QWidget:
        card = QWidget()
        card.setStyleSheet(f"background:{C['surface']}; border-radius:6px;")
        layout = QVBoxLayout(card)
        layout.setContentsMargins(16, 12, 16, 12)
        layout.setSpacing(4)
        layout.addWidget(_mk_lbl("LOG", C["muted"], fs=9, bold=True))
        self._log_text = QTextEdit()
        self._log_text.setReadOnly(True)
        self._log_text.setFixedHeight(140)
        self._log_text.setStyleSheet(f"""
            QTextEdit {{
                background:{C['bg']}; color:{C['muted']};
                border:none; font-family:monospace; font-size:9px;
            }}
        """)
        layout.addWidget(self._log_text)
        return card

    # ------------------------------------------------------------------
    # System info
    # ------------------------------------------------------------------

    def _load_system_info(self):
        def load():
            cpu  = _get_cpu_brand()
            spec = _get_cpu_name()
            ram  = _get_ram_info()
            gpu  = _get_gpu_name()
            os_s = _get_os_info()
            QTimer.singleShot(0, lambda: self._set_sysinfo(cpu, spec, ram, gpu, os_s))
        threading.Thread(target=load, daemon=True).start()

    def _set_sysinfo(self, cpu, spec, ram, gpu, os_s):
        self._cpu_lbl.setText(f"CPU: {cpu}  ({spec})")
        self._ram_lbl.setText(f"RAM: {ram}")
        self._gpu_lbl.setText(f"GPU: {gpu}")
        self._os_lbl .setText(f"OS: {os_s}")

    def _browse_disk(self):
        d = QFileDialog.getExistingDirectory(self, "Select disk test directory")
        if d:
            self._disk_edit.setText(d)

    # ------------------------------------------------------------------
    # Benchmark runner
    # ------------------------------------------------------------------

    def _start_benchmarks(self):
        selected = [t for t in TESTS if self._checkboxes[t].isChecked()]
        if not selected:
            QMessageBox.warning(self, "No tests selected",
                                "Please select at least one benchmark.")
            return

        self._run_btn.setText("◼  Stop")
        self._run_btn.setStyleSheet(
            self._run_btn.styleSheet().replace(C["accent"], C["error"])
                                      .replace(C["accent_h"], C["error"]))
        try:
            self._run_btn.clicked.disconnect()
        except Exception:
            pass
        self._run_btn.clicked.connect(self._stop_benchmarks)
        self._export_btn.setEnabled(False)

        for row in self._score_rows.values():
            row.set_progress(0.0, "Waiting …")
        self._overall_gauge.set(0, OVERALL_MAX, "pts")
        self._log_text.clear()
        self._results = {}
        self._status.showMessage("Running benchmarks …")

        self._worker = BenchmarkWorker(selected, self._disk_edit.text())
        self._worker.sig_progress.connect(self._on_progress)
        self._worker.sig_result  .connect(self._on_result)
        self._worker.sig_status  .connect(self._status.showMessage)
        self._worker.sig_done    .connect(self._on_done)
        self._worker.sig_error   .connect(self._on_error)
        self._worker.start()

    def _stop_benchmarks(self):
        if self._worker:
            self._worker.stop()
        self._status.showMessage("Stopping …")

    def _on_progress(self, test: str, msg: str, frac: float):
        self._score_rows[test].set_progress(min(frac + 0.05, 0.95), msg)
        self._log_text.append(f"[{TEST_LABELS[test]}] {msg}")

    def _on_result(self, test: str, result):
        row = self._score_rows[test]
        if result.available:
            row.set_score(result.score, result.summary)
        else:
            row.set_score(0, result.error or "Not available", unavailable=True)
        self._log_text.append(
            f"[{TEST_LABELS[test]}] Score: {result.score}  {result.summary}")

    def _on_done(self, results: dict):
        self._results = results
        total = sum(r.score for r in results.values() if r.available)
        avail_max = sum(MAX_SCORES[t] for t, r in results.items() if r.available) or OVERALL_MAX
        self._overall_gauge.set(total, avail_max, "pts")
        self._reset_run_btn()
        self._export_btn.setEnabled(True)
        self._status.showMessage(f"Benchmark complete — Overall score: {total}")
        self._log_text.append(f"\nOverall score: {total}")

    def _on_error(self, msg: str):
        QMessageBox.critical(self, "Benchmark Error", msg)
        self._reset_run_btn()
        self._status.showMessage("Error occurred")

    def _reset_run_btn(self):
        self._run_btn.setText("▶  Run Benchmarks")
        self._run_btn.setStyleSheet(
            self._run_btn.styleSheet().replace(C["error"], C["accent"]))
        try:
            self._run_btn.clicked.disconnect()
        except Exception:
            pass
        self._run_btn.clicked.connect(self._start_benchmarks)

    # ------------------------------------------------------------------
    # Runtime setup
    # ------------------------------------------------------------------

    def _check_runtimes(self):
        """Detect optional runtimes in a background thread — never blocks the event loop."""
        def _detect():
            try:
                from runtime import get_recommendations, runtime_setup_skipped
                if runtime_setup_skipped():
                    return
                recs = [r for r in get_recommendations() if not r.is_installed()]
                if recs:
                    QTimer.singleShot(0, lambda: RuntimeSetupDialog(self, recs).exec())
            except Exception:
                pass
        threading.Thread(target=_detect, daemon=True).start()

    def _open_runtime_setup(self):
        from runtime import get_recommendations
        recs = [r for r in get_recommendations() if not r.is_installed()]
        if not recs:
            QMessageBox.information(
                self, "Runtime Setup",
                "All recommended runtime packages are already installed.\n"
                "GPU and NPU benchmarks are ready to run.")
            return
        RuntimeSetupDialog(self, recs).exec()

    # ------------------------------------------------------------------
    # Export
    # ------------------------------------------------------------------

    def _export_json(self):
        if not self._results:
            QMessageBox.information(self, "No results", "Run the benchmarks first.")
            return
        import json
        path, _ = QFileDialog.getSaveFileName(
            self, "Export Results", "", "JSON (*.json);;All files (*)")
        if not path:
            return
        data = {
            name: {"score": r.score, "available": r.available,
                   "summary": r.summary, "metrics": r.metrics}
            for name, r in self._results.items()
        }
        data["overall"] = sum(r.score for r in self._results.values() if r.available)
        with open(path, "w") as f:
            json.dump(data, f, indent=2)
        self._status.showMessage(f"Exported to {path}")

    def _export_text(self):
        if not self._results:
            QMessageBox.information(self, "No results", "Run the benchmarks first.")
            return
        path, _ = QFileDialog.getSaveFileName(
            self, "Export Results", "", "Text (*.txt);;All files (*)")
        if not path:
            return
        lines = ["SynBench Results", "=" * 50, ""]
        for name, r in self._results.items():
            avail = "N/A" if not r.available else f"{r.score} pts"
            lines.append(f"{TEST_LABELS.get(name, name):6s}: {avail:10s}  {r.summary}")
        total = sum(r.score for r in self._results.values() if r.available)
        lines += ["", f"OVERALL: {total} pts"]
        with open(path, "w") as f:
            f.write("\n".join(lines))
        self._status.showMessage(f"Exported to {path}")

    # ------------------------------------------------------------------
    # About
    # ------------------------------------------------------------------

    def _show_about(self):
        QMessageBox.about(
            self, "About SynBench",
            "SynBench v1.0\n\n"
            "Synthetic system benchmark for CPU, GPU, NPU, RAM, and Disk.\n\n"
            "Scoring is calibrated against a Ryzen 5 5600 reference system.\n\n"
            "Run with --cli for command-line / automation mode.")
