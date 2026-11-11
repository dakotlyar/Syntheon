# SPDX-License-Identifier: GPL-3.0-or-later
# The Storage test's per-OS async I/O engines (Phase 4b design §4.1, D7).
# The sync engine ships everywhere (ARM64 / Q1T1 / the fallback); the io_uring,
# libaio and overlapped engines are added by tasks 6/7/9.
target_sources(syntheon_test_storage PRIVATE
    engine.h engine_sync.cpp
)
