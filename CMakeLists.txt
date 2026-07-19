cmake_minimum_required(VERSION 3.10)
project(h3x C)

find_package(Threads REQUIRED)

# Pull in h2o's build. EXCLUDE_FROM_ALL means only the libraries we actually link
# get compiled (libh2o-evloop, which bundles quicly + picotls + the HTTP/3 client),
# not the h2o server or its test suite.
add_subdirectory(deps/h2o EXCLUDE_FROM_ALL)

add_executable(h3x
    src/main.c
    src/worker.c
    src/request.c
    src/tls.c
    src/stats.c)

# Select the evloop backend, exactly as h2o compiles its own evloop binaries. Without
# this the socket.h header defaults to libuv (H2O_USE_LIBUV=1) and the link fails on
# uv_* symbols that libh2o-evloop does not provide.
target_compile_definitions(h3x PRIVATE H2O_USE_LIBUV=0)

# libh2o-evloop links OpenSSL / dl / m / pthread and exports OpenSSL's include dir
# PUBLICly, so all of that propagates here. Only the h2o/quicly/picotls headers need
# adding by hand (h2o uses directory-scoped includes rather than exporting them).
target_link_libraries(h3x libh2o-evloop Threads::Threads)
target_include_directories(h3x PRIVATE
    deps/h2o/include
    deps/h2o/deps/quicly/include
    deps/h2o/deps/picotls/include)
