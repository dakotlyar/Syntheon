# SPDX-License-Identifier: GPL-3.0-or-later
qt_add_library(syntheon_storage STATIC
    database.h database.cpp
    run_repository.h run_repository.cpp
    settings_store.h settings_store.cpp
    storage.h storage.cpp
    sqlite_run_sink.h sqlite_run_sink.cpp
    recovery.h recovery.cpp
    instance_paths.h instance_paths.cpp
    legacy_names.h
    legacy_migration.h legacy_migration.cpp
)

qt_add_resources(syntheon_storage "syntheon_migrations"
    PREFIX "/syntheon"
    FILES
        migrations/0001_init.sql
)

target_link_libraries(syntheon_storage PUBLIC syntheon_core Qt6::Sql)
