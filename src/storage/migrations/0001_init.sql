-- SPDX-License-Identifier: GPL-3.0-or-later
-- SynBench history database, schema version 1 (Phase 1 design §4).
-- Applied by the migration framework in its own transaction; the framework
-- sets PRAGMA user_version afterwards. Never edit a shipped migration: add a
-- new numbered script instead.

CREATE TABLE runs (
  id            INTEGER PRIMARY KEY,
  uuid          TEXT NOT NULL UNIQUE,
  kind          TEXT NOT NULL CHECK (kind IN ('standard','exclusive')),
  status        TEXT NOT NULL CHECK (status IN ('running','completed','stopped','interrupted','recovered')),
  started_at    TEXT NOT NULL,            -- UTC ISO-8601 with ms
  finished_at   TEXT,
  app_version   TEXT NOT NULL,            -- 0.4.3
  app_build     TEXT NOT NULL,            -- os-arch + git sha
  machine_label TEXT NOT NULL,            -- History "Machine" column
  overall_score REAL,
  system_json   TEXT NOT NULL             -- SystemSnapshot, versioned JSON (sysinfo_version)
);
CREATE INDEX runs_started ON runs(started_at DESC);

CREATE TABLE test_results (
  id              INTEGER PRIMARY KEY,
  run_id          INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  position        INTEGER NOT NULL,
  test_id         TEXT NOT NULL,
  test_version    INTEGER NOT NULL,
  formula_version INTEGER NOT NULL,
  status          TEXT NOT NULL,          -- Status enum, lower-case
  status_reason   TEXT,
  score           REAL,
  options_json    TEXT NOT NULL,
  started_at      TEXT,
  duration_ms     INTEGER,
  UNIQUE (run_id, test_id)
);

CREATE TABLE sub_results (
  id INTEGER PRIMARY KEY,
  test_result_id INTEGER NOT NULL REFERENCES test_results(id) ON DELETE CASCADE,
  sub_id TEXT NOT NULL, status TEXT NOT NULL, status_reason TEXT, score REAL,
  UNIQUE (test_result_id, sub_id)
);

CREATE TABLE metrics (
  id INTEGER PRIMARY KEY,
  test_result_id INTEGER NOT NULL REFERENCES test_results(id) ON DELETE CASCADE,
  sub_id TEXT, key TEXT NOT NULL, label TEXT NOT NULL,
  value_num REAL, value_text TEXT,
  unit TEXT NOT NULL DEFAULT '', direction TEXT NOT NULL, headline INTEGER NOT NULL DEFAULT 0,
  position INTEGER NOT NULL,
  CHECK ((value_num IS NULL) <> (value_text IS NULL)),
  UNIQUE (test_result_id, key)
);

CREATE TABLE series (
  id INTEGER PRIMARY KEY,
  test_result_id INTEGER NOT NULL REFERENCES test_results(id) ON DELETE CASCADE,
  key TEXT NOT NULL, label TEXT NOT NULL, x_label TEXT, x_unit TEXT, y_unit TEXT,
  points BLOB NOT NULL                    -- packed little-endian float64 pairs
);

CREATE TABLE checkpoints (
  id INTEGER PRIMARY KEY,
  run_id INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  test_id TEXT NOT NULL, seq INTEGER NOT NULL, created_at TEXT NOT NULL,
  payload BLOB NOT NULL,                  -- CBOR, owned by the test
  UNIQUE (run_id, test_id, seq)
);

CREATE TABLE run_log (
  id INTEGER PRIMARY KEY,
  run_id INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  ts TEXT NOT NULL, level TEXT NOT NULL, test_id TEXT, message TEXT NOT NULL
);
CREATE INDEX run_log_run ON run_log(run_id);

CREATE TABLE scratch_paths (
  id INTEGER PRIMARY KEY,
  run_id INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  path TEXT NOT NULL, created_at TEXT NOT NULL
);
