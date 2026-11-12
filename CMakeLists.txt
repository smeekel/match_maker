cmake_minimum_required(VERSION 3.25)
project(matchmaker LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

set(MM_DEPS "${CMAKE_BINARY_DIR}/deps")
file(MAKE_DIRECTORY "${MM_DEPS}")

function(mm_fetch name url archive)
  if(NOT EXISTS "${archive}")
    message(STATUS "Downloading ${name}")
    file(DOWNLOAD "${url}" "${archive}" SHOW_PROGRESS STATUS mm_st TIMEOUT 900)
    list(GET mm_st 0 mm_code)
    if(NOT mm_code EQUAL 0)
      list(GET mm_st 1 mm_msg)
      message(FATAL_ERROR "Failed to download ${url}: ${mm_msg}")
    endif()
  endif()
endfunction()

set(BOOST_ARCHIVE "${MM_DEPS}/boost-1.90.0-cmake.tar.xz")
mm_fetch(Boost
  "https://github.com/boostorg/boost/releases/download/boost-1.90.0/boost-1.90.0-cmake.tar.xz"
  "${BOOST_ARCHIVE}")
if(NOT EXISTS "${MM_DEPS}/boost-1.90.0/libs/config/include/boost/version.hpp")
  file(ARCHIVE_EXTRACT INPUT "${BOOST_ARCHIVE}" DESTINATION "${MM_DEPS}")
endif()
set(BOOST_INCLUDE_LIBRARIES beast asio CACHE STRING "" FORCE)
add_subdirectory("${MM_DEPS}/boost-1.90.0" "${CMAKE_BINARY_DIR}/_boost" EXCLUDE_FROM_ALL)

set(SQLITE_ARCHIVE "${MM_DEPS}/sqlite-amalgamation-3500100.zip")
mm_fetch(SQLite
  "https://www.sqlite.org/2025/sqlite-amalgamation-3500100.zip"
  "${SQLITE_ARCHIVE}")
if(NOT EXISTS "${MM_DEPS}/sqlite-amalgamation-3500100/sqlite3.c")
  file(ARCHIVE_EXTRACT INPUT "${SQLITE_ARCHIVE}" DESTINATION "${MM_DEPS}")
endif()

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(doctest
  URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(zlib
  URL https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json doctest zlib)

if(WIN32)
  set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64" CACHE PATH "OpenSSL root")
  set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
  set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT_DIR}/lib/VC/x64/MD/libssl.lib")
  set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_ROOT_DIR}/lib/VC/x64/MD/libcrypto.lib")
endif()
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

set(NGTCP2_VERSION 1.26.0)
set(NGTCP2_DIR "${MM_DEPS}/ngtcp2-${NGTCP2_VERSION}")
set(NGTCP2_ARCHIVE "${MM_DEPS}/ngtcp2-${NGTCP2_VERSION}.tar.xz")
mm_fetch(ngtcp2
  "https://github.com/ngtcp2/ngtcp2/releases/download/v${NGTCP2_VERSION}/ngtcp2-${NGTCP2_VERSION}.tar.xz"
  "${NGTCP2_ARCHIVE}")
if(NOT EXISTS "${NGTCP2_DIR}/lib/ngtcp2_conn.c")
  file(ARCHIVE_EXTRACT INPUT "${NGTCP2_ARCHIVE}" DESTINATION "${MM_DEPS}")
endif()

set(NGHTTP3_VERSION 1.19.0)
set(NGHTTP3_DIR "${MM_DEPS}/nghttp3-${NGHTTP3_VERSION}")
set(NGHTTP3_ARCHIVE "${MM_DEPS}/nghttp3-${NGHTTP3_VERSION}.tar.xz")
mm_fetch(nghttp3
  "https://github.com/ngtcp2/nghttp3/releases/download/v${NGHTTP3_VERSION}/nghttp3-${NGHTTP3_VERSION}.tar.xz"
  "${NGHTTP3_ARCHIVE}")
if(NOT EXISTS "${NGHTTP3_DIR}/lib/nghttp3_qpack.c")
  file(ARCHIVE_EXTRACT INPUT "${NGHTTP3_ARCHIVE}" DESTINATION "${MM_DEPS}")
endif()

# Both projects define a top-level `check` target, so they cannot be added as
# subdirectories of one Ninja build. Their library sources are compiled here.
set(MM_QUIC_GEN "${CMAKE_BINARY_DIR}/quic")
if(WIN32)
  # Windows has no ssize_t, and the POSIX byte-swap helpers are absent.
  set(ssize_t ptrdiff_t)
  set(HAVE_DECL_BE64TOH 0)
  set(HAVE_DECL_BSWAP_64 0)
else()
  unset(ssize_t)
  set(HAVE_ARPA_INET_H 1)
  set(HAVE_NETINET_IN_H 1)
  set(HAVE_UNISTD_H 1)
  set(HAVE_ENDIAN_H 1)
  set(HAVE_BYTESWAP_H 1)
  set(HAVE_DECL_BE64TOH 1)
  set(HAVE_DECL_BSWAP_64 1)
endif()
configure_file("${NGTCP2_DIR}/cmakeconfig.h.in" "${MM_QUIC_GEN}/ngtcp2/config.h")
configure_file("${NGHTTP3_DIR}/cmakeconfig.h.in" "${MM_QUIC_GEN}/nghttp3/config.h")
set(PACKAGE_VERSION "${NGTCP2_VERSION}")
set(PACKAGE_VERSION_NUM 0x011a00)
configure_file("${NGTCP2_DIR}/lib/includes/ngtcp2/version.h.in"
  "${MM_QUIC_GEN}/ngtcp2/includes/ngtcp2/version.h" @ONLY)
set(PACKAGE_VERSION "${NGHTTP3_VERSION}")
set(PACKAGE_VERSION_NUM 0x011300)
configure_file("${NGHTTP3_DIR}/lib/includes/nghttp3/version.h.in"
  "${MM_QUIC_GEN}/nghttp3/includes/nghttp3/version.h" @ONLY)

file(GLOB NGTCP2_SOURCES "${NGTCP2_DIR}/lib/*.c")
add_library(ngtcp2 STATIC ${NGTCP2_SOURCES}
  "${NGTCP2_DIR}/crypto/ossl/ossl.c"
  "${NGTCP2_DIR}/crypto/shared.c")
target_include_directories(ngtcp2
  PUBLIC
    "${NGTCP2_DIR}/lib/includes"
    "${MM_QUIC_GEN}/ngtcp2/includes"
    "${NGTCP2_DIR}/crypto/includes"
  PRIVATE
    "${MM_QUIC_GEN}/ngtcp2"
    "${NGTCP2_DIR}/lib"
    "${NGTCP2_DIR}/crypto")
target_compile_definitions(ngtcp2 PUBLIC NGTCP2_STATICLIB PRIVATE HAVE_CONFIG_H BUILDING_NGTCP2)
target_link_libraries(ngtcp2 PUBLIC OpenSSL::SSL OpenSSL::Crypto)
if(WIN32)
  target_link_libraries(ngtcp2 PUBLIC ws2_32)
endif()

file(GLOB NGHTTP3_SOURCES "${NGHTTP3_DIR}/lib/*.c")
add_library(nghttp3 STATIC ${NGHTTP3_SOURCES} "${NGHTTP3_DIR}/lib/sfparse/sfparse.c")
target_include_directories(nghttp3
  PUBLIC
    "${NGHTTP3_DIR}/lib/includes"
    "${MM_QUIC_GEN}/nghttp3/includes"
  PRIVATE
    "${MM_QUIC_GEN}/nghttp3"
    "${NGHTTP3_DIR}/lib")
target_compile_definitions(nghttp3 PUBLIC NGHTTP3_STATICLIB PRIVATE HAVE_CONFIG_H BUILDING_NGHTTP3)
set_target_properties(ngtcp2 nghttp3 PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS ON)
if(MSVC)
  target_compile_options(ngtcp2 PRIVATE /W0)
  target_compile_options(nghttp3 PRIVATE /W0)
endif()

add_library(sqlite3 STATIC "${MM_DEPS}/sqlite-amalgamation-3500100/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "${MM_DEPS}/sqlite-amalgamation-3500100")
target_compile_definitions(sqlite3 PRIVATE
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_DQS=0
  SQLITE_THREADSAFE=1
  SQLITE_DEFAULT_MEMSTATUS=0)
if(MSVC)
  target_compile_options(sqlite3 PRIVATE /W0)
endif()

set(MM_SOURCES
  src/error.cpp
  src/crypto.cpp
  src/jwt.cpp
  src/util.cpp
  src/sqlite_store.cpp
  src/catalog.cpp
  src/lobby.cpp
  src/relay.cpp
  src/log.cpp
  src/admin.cpp
  src/h3.cpp
  src/wt.cpp
  src/net.cpp)

# Re-apply after dependencies. A later add_library copies this value.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(matchmaker_core STATIC ${MM_SOURCES})
target_include_directories(matchmaker_core PUBLIC
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/include")
target_compile_definitions(matchmaker_core PUBLIC
  BOOST_ALL_NO_LIB
  BOOST_ASIO_NO_DEPRECATED
  NOMINMAX
  MATCHMAKER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(WIN32)
  target_compile_definitions(matchmaker_core PUBLIC
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN)
endif()
target_link_libraries(matchmaker_core PUBLIC
  Boost::beast
  nlohmann_json::nlohmann_json
  sqlite3
  ngtcp2
  nghttp3
  OpenSSL::SSL
  OpenSSL::Crypto
  zlibstatic
  Threads::Threads)
if(WIN32)
  target_link_libraries(matchmaker_core PUBLIC ws2_32 crypt32 bcrypt advapi32)
endif()
if(MSVC)
  target_compile_options(matchmaker_core PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc /bigobj)
endif()

add_library(matchmaker_client STATIC src/client.cpp)
target_link_libraries(matchmaker_client PUBLIC matchmaker_core)
if(MSVC)
  target_compile_options(matchmaker_client PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc /bigobj)
endif()

add_executable(matchmaker src/main.cpp)
target_link_libraries(matchmaker PRIVATE matchmaker_core)

add_executable(matchmaker_tests
  tests/main.cpp
  tests/cert.cpp
  tests/store_tests.cpp
  tests/lobby_tests.cpp
  tests/relay_tests.cpp
  tests/integration_tests.cpp
  tests/wt_peer.cpp
  tests/wt_tests.cpp)
target_link_libraries(matchmaker_tests PRIVATE matchmaker_core matchmaker_client doctest::doctest)
if(MSVC)
  target_compile_options(matchmaker PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
  target_compile_options(matchmaker_tests PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc /bigobj)
endif()

function(mm_copy_runtime target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/migrations"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_SOURCE_DIR}/migrations/sqlite"
      "$<TARGET_FILE_DIR:${target}>/migrations/sqlite"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/web/admin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/web/admin/index.html"
      "$<TARGET_FILE_DIR:${target}>/web/admin/index.html")
  if(WIN32)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OPENSSL_ROOT_DIR}/bin/libssl-4-x64.dll"
        "${OPENSSL_ROOT_DIR}/bin/libcrypto-4-x64.dll"
        "$<TARGET_FILE_DIR:${target}>")
  endif()
endfunction()

set_target_properties(matchmaker_core matchmaker_client matchmaker matchmaker_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

mm_copy_runtime(matchmaker)
mm_copy_runtime(matchmaker_tests)

enable_testing()
add_test(NAME matchmaker_tests COMMAND matchmaker_tests)
set_tests_properties(matchmaker_tests PROPERTIES WORKING_DIRECTORY "$<TARGET_FILE_DIR:matchmaker_tests>")
