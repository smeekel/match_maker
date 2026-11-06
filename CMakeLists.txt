cmake_minimum_required(VERSION 3.28)
project(matchmaker LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

if(MSVC)
  # This compiler's C++26 mode is /std:c++latest; it has no /std:c++26 switch.
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "/std:c++latest")
  set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "/std:c++latest")
else()
  set(CMAKE_CXX_STANDARD 26)
endif()

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

set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64" CACHE PATH "OpenSSL root")
set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT_DIR}/lib/VC/x64/MD/libssl.lib")
set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_ROOT_DIR}/lib/VC/x64/MD/libcrypto.lib")
find_package(OpenSSL REQUIRED)

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
  src/queue.cpp
  src/relay.cpp
  src/log.cpp
  src/net.cpp)

add_library(matchmaker_core STATIC ${MM_SOURCES})
target_include_directories(matchmaker_core PUBLIC
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/include")
target_compile_definitions(matchmaker_core PUBLIC
  BOOST_ALL_NO_LIB
  BOOST_ASIO_NO_DEPRECATED
  _WIN32_WINNT=0x0A00
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  MATCHMAKER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(matchmaker_core PUBLIC
  Boost::beast
  nlohmann_json::nlohmann_json
  sqlite3
  OpenSSL::SSL
  OpenSSL::Crypto
  zlibstatic
  ws2_32
  crypt32
  bcrypt
  advapi32)
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
  tests/queue_tests.cpp
  tests/relay_tests.cpp
  tests/integration_tests.cpp)
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
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${OPENSSL_ROOT_DIR}/bin/libssl-4-x64.dll"
      "${OPENSSL_ROOT_DIR}/bin/libcrypto-4-x64.dll"
      "$<TARGET_FILE_DIR:${target}>")
endfunction()

mm_copy_runtime(matchmaker)
mm_copy_runtime(matchmaker_tests)

enable_testing()
add_test(NAME matchmaker_tests COMMAND matchmaker_tests)
set_tests_properties(matchmaker_tests PROPERTIES WORKING_DIRECTORY "$<TARGET_FILE_DIR:matchmaker_tests>")
