# SPDX-License-Identifier: BSD-2-Clause
# Craft's libcurl package excludes Android because of its libpsl dependency.
# S3 only needs HTTP(S), so build a checksum-pinned static curl without libpsl.
function(okular_build_android_curl)
    include(FetchContent)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_STATIC_LIBS ON)
    set(BUILD_CURL_EXE OFF)
    set(BUILD_TESTING OFF)
    set(HTTP_ONLY ON)
    set(CURL_USE_OPENSSL ON)
    set(CURL_USE_LIBPSL OFF)
    set(CURL_USE_LIBSSH2 OFF)
    set(CURL_ZLIB OFF)
    set(CURL_BROTLI OFF)
    set(CURL_ZSTD OFF)
    set(USE_NGHTTP2 OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    FetchContent_Declare(okular_curl
        URL https://curl.se/download/curl-8.14.1.tar.bz2
        URL_HASH SHA256=5760ed3c1a6aac68793fc502114f35c3e088e8cd5c084c2d044abdf646ee48fb
    )
    FetchContent_MakeAvailable(okular_curl)
endfunction()
