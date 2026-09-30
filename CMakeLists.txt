cmake_minimum_required(VERSION 3.20)
project(lugbulk_labels_web CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# --- Crow (header-only web framework) ---
FetchContent_Declare(
    Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG 94a011b9f7c0a991e5382927a2dbe5a7d9a056b8  # v1.2.0 (pinned by commit)
    # Cap request body size (see cmake/patch_crow.cmake).
    PATCH_COMMAND ${CMAKE_COMMAND} -DCROW_SOURCE_DIR=<SOURCE_DIR>
                  -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_crow.cmake
)
set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(Crow)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)      # OAuth token exchange (HTTPS client) + Crow SSL support
find_package(SQLite3 REQUIRED)      # sessions / users / sheets / run log
find_package(CURL REQUIRED)         # outbound HTTPS to Google (OAuth + Sheets API)
find_package(ZLIB REQUIRED)         # unpacking .xlsx workbooks (spreadsheet.cpp, the parity reference)

# Everything but main(), shared by the server and the tests.
add_library(lugbulk_core STATIC
    src/bricklink.cpp
    src/config.cpp
    src/colors.cpp
    src/crypto.cpp
    src/db.cpp
    src/oauth.cpp
    src/ordering.cpp
    src/sheet_layout.cpp
    src/sheet_pivot.cpp
    src/records.cpp
    src/rate_limits.cpp
    src/spreadsheet.cpp
    src/json_check.cpp
    src/label_options.cpp
    src/part_images.cpp
)

target_include_directories(lugbulk_core PUBLIC src)

target_link_libraries(lugbulk_core PUBLIC
    Crow::Crow
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
    CURL::libcurl
    ZLIB::ZLIB
)

add_executable(lugbulk_labels_web src/main.cpp)
target_link_libraries(lugbulk_labels_web PRIVATE lugbulk_core)

# Unit tests: `cmake --build build && ctest --test-dir build`
option(LUGBULK_BUILD_TESTS "Build unit tests" ON)
if(LUGBULK_BUILD_TESTS)
    enable_testing()
    add_executable(lugbulk_tests tests/tests.cpp)
    target_compile_definitions(lugbulk_tests PRIVATE
        LUGBULK_LABEL_SPECS_PATH="${CMAKE_SOURCE_DIR}/data/label_specs.json"
        LUGBULK_SCHEMA_PATH="${CMAKE_SOURCE_DIR}/sql/schema.sql"
        LUGBULK_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures")
    target_link_libraries(lugbulk_tests PRIVATE lugbulk_core)
    add_test(NAME lugbulk_tests COMMAND lugbulk_tests)

    # Golden JSON for the browser code's parity test (tests/js/): run as
    # `lugbulk_golden <dir>`; the Docker build writes build/golden.
    add_executable(lugbulk_golden tests/golden.cpp)
    target_compile_definitions(lugbulk_golden PRIVATE
        LUGBULK_LABEL_SPECS_PATH="${CMAKE_SOURCE_DIR}/data/label_specs.json"
        LUGBULK_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures")
    target_link_libraries(lugbulk_golden PRIVATE lugbulk_core)
endif()
