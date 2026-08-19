cmake_minimum_required(VERSION 3.20)
project(lugbulk_labels_web CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# --- Crow (header-only web framework) ---
FetchContent_Declare(
    Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG v1.2.0
)
set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(Crow)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)      # OAuth token exchange (HTTPS client) + Crow SSL support
find_package(SQLite3 REQUIRED)      # sessions / users / sheets / run log
find_package(CURL REQUIRED)         # outbound HTTPS to Google (OAuth + Sheets API)

# --- PDF library: PoDoFo ---
# No upstream CMake config on Debian (libpodofo-dev), so locate by hand.
# Chosen over libharu because libharu isn't packaged for Debian bookworm
# (our Docker base) — building it from source isn't worth it here.
find_path(PODOFO_INCLUDE_DIR podofo/podofo.h)
find_library(PODOFO_LIBRARY NAMES podofo)
if(NOT PODOFO_INCLUDE_DIR OR NOT PODOFO_LIBRARY)
    message(FATAL_ERROR "PoDoFo (podofo/podofo.h / libpodofo) not found — install libpodofo-dev")
endif()

add_executable(lugbulk_labels_web
    src/main.cpp
    src/config.cpp
    src/crypto.cpp
    src/db.cpp
    src/oauth.cpp
    src/sheet_pivot.cpp
    src/reports.cpp
    src/labels_pdf.cpp
    src/pdf_text.cpp
)

target_include_directories(lugbulk_labels_web PRIVATE
    include
    ${PODOFO_INCLUDE_DIR}
)

target_link_libraries(lugbulk_labels_web PRIVATE
    Crow::Crow
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
    CURL::libcurl
    ${PODOFO_LIBRARY}
)
