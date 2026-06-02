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
    src/salesforce_config.cpp
    src/salesforce_auth.cpp
    src/salesforce_http_client.cpp
    src/salesforce_session.cpp
    src/salesforce_types.cpp
    src/salesforce_describe.cpp
    src/salesforce_query.cpp
    src/salesforce_value.cpp
    src/salesforce_scan.cpp
    src/salesforce_soql.cpp
    src/salesforce_quota.cpp
    src/salesforce_diag.cpp
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
