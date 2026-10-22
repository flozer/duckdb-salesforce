cmake_minimum_required(VERSION 3.5)

# Included from DuckDB's extension build system via extension_config.cmake /
# duckdb_extension_load. Direct standalone invocation will not produce a
# working .duckdb_extension — use `make release`.

set(TARGET_NAME salesforce)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

# Dual-line support (1.5.x + 2.0): flag the API generation so the sources can
# gate version-divergent call sites. DUCKDB_VERSION comes from the DuckDB
# build itself (e.g. "v1.5.6" / "v2.0.0").
# SF_DUCKDB_HAS_IDENTIFIER: duckdb::Identifier + GetIdentifierName exist on
# 2.0 and on the 1.5-variegata line from v1.5.6 on — but NOT on v1.5.4/v1.5.5.
if(DEFINED DUCKDB_VERSION)
    if(DUCKDB_VERSION MATCHES "^v?2\\.")
        add_compile_definitions(SF_DUCKDB_V2=1 SF_DUCKDB_HAS_IDENTIFIER=1)
    elseif(DUCKDB_VERSION MATCHES "^v?1\\.5\\.[6-9]")
        add_compile_definitions(SF_DUCKDB_HAS_IDENTIFIER=1)
    endif()
endif()

project(${TARGET_NAME} CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include_directories(src/include)

# v0.1 scaffold sources only. Auth / HTTP / scanner / query-builder modules
# are added by their respective milestone issues (#3, #4, #5-#9).
set(EXTENSION_SOURCES
    src/salesforce_extension.cpp
    src/salesforce_agg_optimizer.cpp
    src/salesforce_bulk_resume.cpp
    src/salesforce_scan_user.cpp
    src/salesforce_delete_sync.cpp
    src/salesforce_storage.cpp
    src/salesforce_config.cpp
    src/salesforce_auth.cpp
    src/salesforce_http_client.cpp
    src/salesforce_session.cpp
    src/salesforce_types.cpp
    src/salesforce_describe.cpp
    src/salesforce_query.cpp
    src/salesforce_report.cpp
    src/salesforce_metadata_engine.cpp
    src/salesforce_value.cpp
    src/salesforce_scan.cpp
    src/salesforce_soql.cpp
    src/salesforce_quota.cpp
    src/salesforce_diag.cpp
    src/salesforce_reldiag.cpp
    src/salesforce_aggregate.cpp
    src/salesforce_metadata.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# --- HTTPS transport: vendored httplib + OpenSSL (issue #4) ------------------
# httplib is header-only and vendored under third_party/httplib. TLS is provided
# by OpenSSL (pulled via vcpkg manifest mode — see vcpkg.json). Certificate
# verification is always ON in the client; there is no insecure build flag.
find_package(OpenSSL REQUIRED)

set(SF_HTTPLIB_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib")

foreach(_t ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
    target_include_directories(${_t} PRIVATE ${SF_HTTPLIB_INCLUDE})
    target_compile_definitions(${_t} PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
    target_link_libraries(${_t} OpenSSL::SSL OpenSSL::Crypto)
    if(WIN32)
        # CryptoAPI (system ROOT cert store) + sockets.
        target_link_libraries(${_t} crypt32 ws2_32)
    endif()
endforeach()

install(TARGETS ${EXTENSION_NAME}
    EXPORT  "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
