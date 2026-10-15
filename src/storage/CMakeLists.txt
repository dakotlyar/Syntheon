qt_add_library(synbench_storage STATIC
    database.h database.cpp
    run_repository.h run_repository.cpp
    settings_store.h settings_store.cpp
    storage.h storage.cpp
)

qt_add_resources(synbench_storage "synbench_migrations"
    PREFIX "/synbench"
    FILES
        migrations/0001_init.sql
)

target_link_libraries(synbench_storage PUBLIC synbench_core Qt6::Sql)
