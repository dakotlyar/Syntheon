#!/bin/sh
# SynBench launcher – finds a Python 3 installation with tkinter and runs synbench.py.
# All arguments are forwarded (e.g. --cli --tests cpu ram --json).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ---- Prefer uv ----
# uv-managed Pythons bundle tkinter and handle dependency resolution automatically.
if command -v uv >/dev/null 2>&1; then
    exec uv run --python 3.10 \
        --with numpy \
        --with psutil \
        --with PySide6 \
        "$SCRIPT_DIR/synbench.py" "$@"
fi

# ---- Fall back to a system Python with PySide6 ----
PYTHON=""
for cand in python3.13 python3.12 python3.11 python3.10 python3.9 python3; do
    if command -v "$cand" >/dev/null 2>&1; then
        if "$cand" -c "import PySide6, numpy, psutil" 2>/dev/null; then
            PYTHON="$cand"
            break
        fi
    fi
done

if [ -z "$PYTHON" ]; then
    # Fallback: any python3 with numpy (CLI-only)
    for cand in python3.13 python3.12 python3.11 python3.10 python3.9 python3; do
        if command -v "$cand" >/dev/null 2>&1; then
            if "$cand" -c "import numpy, psutil" 2>/dev/null; then
                PYTHON="$cand"
                echo "Warning: PySide6 not found. Launching in CLI mode." >&2
                set -- --cli "$@"
                break
            fi
        fi
    done
fi

if [ -z "$PYTHON" ]; then
    echo "Error: No suitable Python 3 found." >&2
    echo "Install uv and re-run: curl -LsSf https://astral.sh/uv/install.sh | sh" >&2
    exit 1
fi

exec "$PYTHON" "$SCRIPT_DIR/synbench.py" "$@"
