# DuckDB out-of-tree extension config.
# Consumed by extension-ci-tools/makefiles/duckdb_extension.Makefile.

duckdb_extension_load(salesforce
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# v2 (cyanoptera): static linkage is opt-in; without this the unittest/shell
# binaries do not register salesforce and every test skips on 'require'.
duckdb_extension_statically_link(salesforce)
