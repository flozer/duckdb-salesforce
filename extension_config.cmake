# DuckDB out-of-tree extension config.
# Consumed by extension-ci-tools/makefiles/duckdb_extension.Makefile.

duckdb_extension_load(salesforce
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# v2 (cyanoptera): static linkage is opt-in; without this the unittest/shell
# binaries do not register salesforce and every test skips on 'require'.
# 2.0 makes static linkage opt-in; 1.5.x links every built extension and does
# not know this function.
if(COMMAND duckdb_extension_statically_link)
    duckdb_extension_statically_link(salesforce)
endif()
