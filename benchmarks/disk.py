"""
Disk I/O benchmark: sequential read/write and random 4 K read/write.

Scoring reference: PCIe 4.0 NVMe SSD (~3 500 MB/s seq, ~700 K 4K IOPS) ≈ 200 disk points.
SATA SSD (~550 MB/s seq, ~90 K 4K IOPS) ≈ 100 points.

Sequential write includes fsync() so results represent real device speed.
On Linux, posix_fadvise(DONTNEED) evicts the page cache before read tests.
On Windows, reads go via the OS cache (no equivalent unprivileged call exists).
"""

import ctypes
import ctypes.util
import os
import platform
import random
import tempfile
import time
from typing import Callable, Dict, Optional

from . import BenchmarkResult


# ---------------------------------------------------------------------------
# Page-cache eviction — Linux only.
# ---------------------------------------------------------------------------
_POSIX_FADV_DONTNEED = 4


def _drop_file_cache(fd: int, size: int) -> None:
    """Ask the Linux kernel to evict a file's pages from the page cache."""
    if platform.system() != "Linux":
        return
    try:
        libc_name = ctypes.util.find_library("c") or "libc.so.6"
        libc = ctypes.CDLL(libc_name, use_errno=True)
        libc.posix_fadvise(
            ctypes.c_int(fd), ctypes.c_int64(0),
            ctypes.c_int64(size), ctypes.c_int(_POSIX_FADV_DONTNEED),
        )
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Cross-platform positional I/O.
# os.pread / os.pwrite exist on POSIX but NOT on Windows.
# ---------------------------------------------------------------------------
_HAS_PREAD = hasattr(os, "pread")


def _pread(fd: int, size: int, offset: int) -> bytes:
    if _HAS_PREAD:
        return os.pread(fd, size, offset)  # type: ignore[attr-defined]
    os.lseek(fd, offset, os.SEEK_SET)
    return os.read(fd, size)


def _pwrite(fd: int, data: bytes, offset: int) -> None:
    if _HAS_PREAD:
        os.pwrite(fd, data, offset)  # type: ignore[attr-defined]
    else:
        os.lseek(fd, offset, os.SEEK_SET)
        os.write(fd, data)


# ---------------------------------------------------------------------------
# Calibration
# ---------------------------------------------------------------------------
_SEQ_READ_REF   = 3_500.0    # MB/s
_SEQ_WRITE_REF  = 3_000.0    # MB/s
_RAND_READ_REF  = 700_000    # IOPS (4 K random)
_RAND_WRITE_REF = 600_000    # IOPS (4 K random)

_SEQ_READ_WEIGHT   = 50
_SEQ_WRITE_WEIGHT  = 50
_RAND_READ_WEIGHT  = 50
_RAND_WRITE_WEIGHT = 50

_FILE_SIZE  = 256 * 1024 * 1024   # 256 MB
_CHUNK_SIZE = 1   * 1024 * 1024   # 1 MB chunks
_RAND_OPS   = 2_000               # random 4 K operations
_BLOCK_4K   = 4  * 1024           # 4 KB


# ---------------------------------------------------------------------------
# Test directory selection
# ---------------------------------------------------------------------------

def _find_real_disk_dir() -> str:
    """Return a writable directory on a real (non-RAM) filesystem."""
    system = platform.system()

    if system == "Windows":
        # Windows temp / home dirs are always on real NTFS/exFAT — no tmpfs.
        candidates = [
            os.path.expanduser("~"),
            os.environ.get("USERPROFILE", ""),
            os.environ.get("LOCALAPPDATA", ""),
            os.environ.get("TEMP", ""),
            "C:\\",
        ]
        for path in candidates:
            if not path or not os.path.isdir(path):
                continue
            probe = os.path.join(path, ".synbench_probe")
            try:
                with open(probe, "wb") as f:
                    f.write(b"x")
                os.unlink(probe)
                return path
            except OSError:
                continue
        return os.path.expanduser("~")

    # Linux / macOS: avoid tmpfs / ramfs mounts.
    candidates = [
        os.path.expanduser("~"),
        os.path.expanduser("~/Documents"),
        "/var/tmp",   # usually on disk, unlike /tmp
        "/tmp",
    ]
    skip_types = {"tmpfs", "ramfs", "devtmpfs", "hugetlbfs"}

    if system == "Linux":
        mounts: Dict[str, str] = {}
        try:
            with open("/proc/mounts") as f:
                for line in f:
                    parts = line.split()
                    if len(parts) >= 3:
                        mounts[parts[1]] = parts[2]  # mountpoint → fstype
        except OSError:
            pass

        def _fs_type(path: str) -> str:
            best = ""
            for mp in mounts:
                if path.startswith(mp) and len(mp) > len(best):
                    best = mp
            return mounts.get(best, "")

        for d in candidates:
            if os.path.isdir(d) and _fs_type(d) not in skip_types:
                return d
    else:
        # macOS — no tmpfs concern for home dir
        for d in candidates:
            if os.path.isdir(d):
                return d

    return os.path.expanduser("~")


# ---------------------------------------------------------------------------
# Benchmark class
# ---------------------------------------------------------------------------

class DiskBenchmark:
    def __init__(
        self,
        callback: Optional[Callable[[str], None]] = None,
        test_dir: Optional[str] = None,
    ):
        self.cb = callback or (lambda msg: None)
        self._test_dir  = test_dir or _find_real_disk_dir()
        self._tmp_path: Optional[str] = None

    # ------------------------------------------------------------------
    def _seq_write(self) -> float:
        """Write _FILE_SIZE bytes in _CHUNK_SIZE chunks, fsync, then drop cache."""
        self.cb("Sequential write test (256 MB) …")
        payload   = os.urandom(_CHUNK_SIZE)
        n_chunks  = _FILE_SIZE // _CHUNK_SIZE

        self.cb(f"Test directory: {self._test_dir}")
        fd, path = tempfile.mkstemp(prefix="synbench_", dir=self._test_dir)
        self._tmp_path = path
        try:
            start = time.perf_counter()
            for _ in range(n_chunks):
                os.write(fd, payload)
            os.fsync(fd)
            elapsed = time.perf_counter() - start
            _drop_file_cache(fd, _FILE_SIZE)
        finally:
            os.close(fd)

        return (_FILE_SIZE / 1e6) / elapsed  # MB/s

    def _seq_read(self) -> float:
        """Read the file back — page cache was evicted by _seq_write on Linux."""
        if not self._tmp_path or not os.path.exists(self._tmp_path):
            return 0.0
        self.cb("Sequential read test (256 MB) …")
        total = 0
        start = time.perf_counter()
        with open(self._tmp_path, "rb", buffering=0) as f:
            while True:
                chunk = f.read(_CHUNK_SIZE)
                if not chunk:
                    break
                total += len(chunk)
        elapsed = time.perf_counter() - start
        return (total / 1e6) / elapsed  # MB/s

    def _rand_write(self) -> float:
        """Random 4 K writes at scattered offsets, then drop cache."""
        if not self._tmp_path or not os.path.exists(self._tmp_path):
            return 0.0
        self.cb(f"Random 4 K write test ({_RAND_OPS} ops) …")
        max_offset = _FILE_SIZE - _BLOCK_4K
        payload    = os.urandom(_BLOCK_4K)
        offsets    = [random.randrange(0, max_offset, _BLOCK_4K) for _ in range(_RAND_OPS)]

        flags = os.O_RDWR | (os.O_BINARY if hasattr(os, "O_BINARY") else 0)
        fd = os.open(self._tmp_path, flags)
        try:
            start = time.perf_counter()
            for off in offsets:
                _pwrite(fd, payload, off)
            os.fsync(fd)
            elapsed = time.perf_counter() - start
            _drop_file_cache(fd, _FILE_SIZE)
        finally:
            os.close(fd)

        return _RAND_OPS / elapsed  # IOPS

    def _rand_read(self) -> float:
        """Random 4 K reads (page cache evicted on Linux by previous step)."""
        if not self._tmp_path or not os.path.exists(self._tmp_path):
            return 0.0
        self.cb(f"Random 4 K read test ({_RAND_OPS} ops) …")
        max_offset = _FILE_SIZE - _BLOCK_4K
        offsets    = [random.randrange(0, max_offset, _BLOCK_4K) for _ in range(_RAND_OPS)]

        flags = os.O_RDONLY | (os.O_BINARY if hasattr(os, "O_BINARY") else 0)
        fd = os.open(self._tmp_path, flags)
        try:
            start = time.perf_counter()
            for off in offsets:
                _pread(fd, _BLOCK_4K, off)
            elapsed = time.perf_counter() - start
        finally:
            os.close(fd)

        return _RAND_OPS / elapsed  # IOPS

    def _cleanup(self) -> None:
        if self._tmp_path and os.path.exists(self._tmp_path):
            try:
                os.unlink(self._tmp_path)
            except OSError:
                pass

    # ------------------------------------------------------------------
    def run(self) -> BenchmarkResult:
        self.cb(f"Starting disk benchmark (dir: {self._test_dir}) …")
        try:
            seq_write        = self._seq_write()
            seq_read         = self._seq_read()
            rand_write_iops  = self._rand_write()
            rand_read_iops   = self._rand_read()
        finally:
            self._cleanup()

        sw_score = min(_SEQ_WRITE_WEIGHT,  int(seq_write       / _SEQ_WRITE_REF  * _SEQ_WRITE_WEIGHT))
        sr_score = min(_SEQ_READ_WEIGHT,   int(seq_read        / _SEQ_READ_REF   * _SEQ_READ_WEIGHT))
        rw_score = min(_RAND_WRITE_WEIGHT, int(rand_write_iops / _RAND_WRITE_REF * _RAND_WRITE_WEIGHT))
        rr_score = min(_RAND_READ_WEIGHT,  int(rand_read_iops  / _RAND_READ_REF  * _RAND_READ_WEIGHT))
        total = sw_score + sr_score + rw_score + rr_score

        summary = (
            f"Seq R {seq_read:.0f} MB/s  "
            f"Seq W {seq_write:.0f} MB/s  "
            f"Rand R {rand_read_iops/1000:.0f}K IOPS  "
            f"Rand W {rand_write_iops/1000:.0f}K IOPS"
        )
        self.cb(f"Disk done — score {total}")

        return BenchmarkResult(
            name="disk",
            score=total,
            metrics={
                "seq_read_mbps":    seq_read,
                "seq_write_mbps":   seq_write,
                "rand_read_iops":   rand_read_iops,
                "rand_write_iops":  rand_write_iops,
                "seq_read_score":   sr_score,
                "seq_write_score":  sw_score,
                "rand_read_score":  rr_score,
                "rand_write_score": rw_score,
            },
            summary=summary,
        )
