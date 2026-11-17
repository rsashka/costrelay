cmake_minimum_required(VERSION 3.20)
project(costrelay VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_link_options("LINKER:--disable-new-dtags")

set(DEPS_ROOT "${CMAKE_SOURCE_DIR}/.deps/root" CACHE PATH "Local extracted dependency prefix")
set(DEPS_LIB "${DEPS_ROOT}/usr/lib/x86_64-linux-gnu")
set(DROGON_PREFIX "${CMAKE_SOURCE_DIR}/.deps/ssl" CACHE PATH "Drogon built with OpenSSL")

if(NOT EXISTS "${DROGON_PREFIX}/include/drogon/drogon.h")
    message(FATAL_ERROR "SSL-enabled Drogon not found under ${DROGON_PREFIX}. Run scripts/bootstrap_deps.sh")
endif()

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_library(SODIUM_LIBRARY NAMES sodium PATHS "${DEPS_LIB}" REQUIRED)
find_library(ZSTD_LIBRARY NAMES zstd REQUIRED)
find_path(SQLITE3_HEADER sqlite3.h
    PATHS "${DEPS_ROOT}/usr/include" REQUIRED)
# Expose only sqlite3.h: .deps/root/usr/include also contains stale Drogon/trantor
# headers from the distro package, which must not shadow the SSL build.
set(SQLITE3_INCLUDE_DIR "${CMAKE_BINARY_DIR}/sqlite-include")
file(MAKE_DIRECTORY "${SQLITE3_INCLUDE_DIR}")
file(COPY "${SQLITE3_HEADER}" DESTINATION "${SQLITE3_INCLUDE_DIR}")
find_library(SQLITE3_LIBRARY NAMES sqlite3
    PATHS "${DEPS_LIB}" REQUIRED)

find_package(yaml-cpp CONFIG REQUIRED
    PATHS "${DEPS_LIB}/cmake/yaml-cpp" NO_DEFAULT_PATH)
find_package(jsoncpp CONFIG REQUIRED
    PATHS "${DEPS_LIB}/cmake/jsoncpp" NO_DEFAULT_PATH)

add_library(drogon SHARED IMPORTED)
set_target_properties(drogon PROPERTIES
    IMPORTED_LOCATION "${DROGON_PREFIX}/lib/libdrogon.so.1.9.14"
    INTERFACE_INCLUDE_DIRECTORIES "${DROGON_PREFIX}/include;${DEPS_ROOT}/usr/include/jsoncpp"
    INTERFACE_LINK_LIBRARIES "trantor;JsonCpp::JsonCpp;yaml-cpp::yaml-cpp;ZLIB::ZLIB;OpenSSL::SSL;OpenSSL::Crypto;Threads::Threads"
)

add_library(trantor SHARED IMPORTED)
set_target_properties(trantor PROPERTIES
    IMPORTED_LOCATION "${DROGON_PREFIX}/lib/libtrantor.so.1.5.29"
    INTERFACE_INCLUDE_DIRECTORIES "${DROGON_PREFIX}/include"
)

set(CMAKE_BUILD_RPATH "${DROGON_PREFIX}/lib;${DEPS_LIB}")
set(CMAKE_INSTALL_RPATH "${DROGON_PREFIX}/lib;${DEPS_LIB}")

add_library(costrelay_core
    src/analytics/ProviderAnalytics.cpp
    src/app/Endpoints.cpp
    src/app/ServiceControl.cpp
    src/config/Config.cpp
    src/auth/Auth.cpp
    src/pricing/PriceCatalog.cpp
    src/pricing/PriceFetcher.cpp
    src/pricing/CostModel.cpp
    src/routing/Router.cpp
    src/routing/ProviderHealth.cpp
    src/routing/ProviderQueue.cpp
    src/portal/PortalProxy.cpp
    src/limits/QuotaManager.cpp
    src/stats/Stats.cpp
    src/stats/StatsDb.cpp
    src/capture/BodyStore.cpp
    src/upstream/CurlClient.cpp
    src/proxy/ProxyService.cpp
    src/util/Http.cpp
    src/util/Process.cpp
    src/util/Strings.cpp
    src/util/Cidr.cpp
    src/util/Paths.cpp
)
target_include_directories(costrelay_core PUBLIC src "${SQLITE3_INCLUDE_DIR}")
target_link_libraries(costrelay_core PUBLIC
    drogon trantor
    JsonCpp::JsonCpp yaml-cpp::yaml-cpp
    CURL::libcurl OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB ${ZSTD_LIBRARY} ${SODIUM_LIBRARY}
    ${SQLITE3_LIBRARY} ${CMAKE_DL_LIBS}
    Threads::Threads
)
target_compile_options(costrelay_core PRIVATE -Wall -Wextra -Wpedantic)

# Шаблон страницы панели живёт отдельным файлом и встраивается в бинарник при
# конфигурации: страницы не зависят от файлов рядом с рабочим каталогом процесса.
set(COSTRELAY_PAGE_TEMPLATE ${CMAKE_CURRENT_SOURCE_DIR}/src/app/assets/page.html)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${COSTRELAY_PAGE_TEMPLATE})
file(READ ${COSTRELAY_PAGE_TEMPLATE} COSTRELAY_PAGE_HTML)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/PageAssets.h
     "// Generated from src/app/assets/page.html.\n#pragma once\ninline constexpr const char* kPageTemplate = R\"PAGE(${COSTRELAY_PAGE_HTML})PAGE\";\n")

add_executable(costrelayd src/app/main.cpp src/app/App.cpp src/app/AppPages.cpp src/app/AppApi.cpp)
target_include_directories(costrelayd PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(costrelayd PRIVATE costrelay_core)
target_compile_options(costrelayd PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(costrelay_tests
        tests/test_provider_analytics.cpp
        tests/test_config.cpp
        tests/test_auth.cpp
        tests/test_pricing.cpp
        tests/test_routing.cpp
        tests/test_capture.cpp
        tests/test_upstream.cpp
        tests/test_proxy.cpp
        tests/test_price_fetcher.cpp
        tests/test_stats.cpp
        tests/test_limits.cpp
        tests/test_statsdb.cpp
        tests/test_util.cpp
        tests/test_app_integration.cpp
    )
    add_dependencies(costrelay_tests costrelayd)
    target_link_libraries(costrelay_tests PRIVATE costrelay_core GTest::gtest GTest::gtest_main)
    target_compile_definitions(costrelay_tests PRIVATE COSTRELAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}" COSTRELAY_BINARY="$<TARGET_FILE:costrelayd>")
    target_compile_options(costrelay_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME costrelay_tests COMMAND costrelay_tests)
    set_tests_properties(costrelay_tests PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=${DROGON_PREFIX}/lib:${DEPS_LIB}")
endif()
