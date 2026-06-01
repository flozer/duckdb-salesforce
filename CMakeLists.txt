cmake_minimum_required(VERSION 3.5)

# Included from DuckDB's extension build system via extension_config.cmake /
# duckdb_extension_load. Direct standalone invocation will not produce a
# working .duckdb_extension — use `make release`.

set(TARGET_NAME salesforce)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME} CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include_directories(src/include)

# v0.1 scaffold sources only. Auth / HTTP / scanner / query-builder modules
# are added by their respective milestone issues (#3, #4, #5-#9).
set(EXTENSION_SOURCES
    src/salesforce_extension.cpp
    src/salesforce_storage.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(TARGETS ${EXTENSION_NAME}
    EXPORT  "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
