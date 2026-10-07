cmake_minimum_required(VERSION 3.17)
project(UnifiedBenchmark)

set(CMAKE_CXX_STANDARD 17)

# Backends whose upstream sources are not redistributed by this repository.
# Their source directories remain usable in an authorized local checkout.
option(ENABLE_GTX "Build the optional GTX backend (requires systems/GTX)" OFF)
option(ENABLE_SORTLEDTON "Build the optional Sortledton backend (requires systems/Sortledton)" OFF)

# Set the build type before adding backends. Release disables assert() checks.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(OpenMP REQUIRED)
find_package(ZLIB REQUIRED)
find_package(TBB REQUIRED)
find_package(Threads REQUIRED)

add_definitions(-DTBB_PREVIEW_CONCURRENT_ORDERED_CONTAINERS=1)

# =========================================================
# Vendored backends are built from source under systems/.
# Rebuild this project after changing backend sources.
# =========================================================

# --- AVB: static library target AVB_Core ---
add_subdirectory(systems/AVBGraph build_avb)

if(ENABLE_GTX)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/systems/GTX/CMakeLists.txt")
        message(FATAL_ERROR "ENABLE_GTX=ON requires the separately obtained systems/GTX source tree")
    endif()
    # Shared library target gtx; link the target, not a prebuilt .so.
    add_subdirectory(systems/GTX build_gtx)
endif()

if(ENABLE_SORTLEDTON)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/systems/Sortledton/CMakeLists.txt")
        message(FATAL_ERROR "ENABLE_SORTLEDTON=ON requires the separately obtained systems/Sortledton source tree")
    endif()
    add_subdirectory(systems/Sortledton build_slt)
endif()

# --- RapidStore (NeoGraph): static library neo_graph, with c_art and neo_bitmap ---
# Add only NeoGraph to avoid unrelated dependencies of the top-level RapidStore project.
add_subdirectory(systems/RapidStore/libraries/NeoGraph build_neo)

# --- LiveGraph (EuroSys'20): shared library livegraph and corelib ---
# Its CMakeLists searches CMAKE_SOURCE_DIR/cmake for FindTBB, which points to the
# superbuild root here; add LiveGraph's cmake directory to the module search path.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/systems/LiveGraph/cmake")
add_subdirectory(systems/LiveGraph build_lg)

# --- Teseo: an autotools project, not a CMake subdirectory ---
# build_teseo.sh builds out of source under systems/Teseo/build; link the resulting archive.
# The custom target invokes the incremental build script when libteseo.a is missing
# or Teseo sources have changed.
set(TESEO_LIB "${CMAKE_CURRENT_SOURCE_DIR}/systems/Teseo/build/libteseo.a")
file(GLOB_RECURSE TESEO_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/systems/Teseo/src/*.cpp")
add_custom_command(
    OUTPUT "${TESEO_LIB}"
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/build_teseo.sh"
    DEPENDS ${TESEO_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/build_teseo.sh"
    COMMENT "Building Teseo via autotools (incremental)"
    VERBATIM
)
add_custom_target(teseo_build DEPENDS "${TESEO_LIB}")
add_library(teseo_interface INTERFACE)
target_include_directories(teseo_interface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/systems/Teseo/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/systems/Teseo/build/include"  # Generated static_configuration.hpp.
)

# =========================================================
# Header interfaces for backends that do not export PUBLIC include directories.
# =========================================================

if(ENABLE_GTX)
    add_library(gtx_interface INTERFACE)
    target_include_directories(gtx_interface INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/systems/GTX/bind"
        "${CMAKE_CURRENT_SOURCE_DIR}/systems/GTX/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/systems/GTX/Libraries/parallel_hashmap"
    )
endif()

if(ENABLE_SORTLEDTON)
    add_library(slt_interface INTERFACE)
    target_include_directories(slt_interface INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/systems/Sortledton"
        "${CMAKE_CURRENT_SOURCE_DIR}/systems/Sortledton/data-structure"
    )
endif()

# NeoGraph headers.
add_library(neo_interface INTERFACE)
target_include_directories(neo_interface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/systems/RapidStore/libraries/NeoGraph"
    "${CMAKE_CURRENT_SOURCE_DIR}/systems/RapidStore/libraries/NeoGraph/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/systems/RapidStore/libraries/NeoGraph/utils"
)

# LiveGraph headers; bind/livegraph.hpp forward-declares core types.
add_library(lg_interface INTERFACE)
target_include_directories(lg_interface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/systems/LiveGraph/bind"
    "${CMAKE_CURRENT_SOURCE_DIR}/systems/LiveGraph/core"
)

# =========================================================
# Build the driver runners.
# =========================================================
file(GLOB BENCH_UTILS_SRC "utils/*.cpp")

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/utils)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_executable(bench_run main.cpp ${BENCH_UTILS_SRC})
# coarse AVB variant: same driver, but links AVB_Core_coarse and compiles with AVB_COARSE
add_executable(bench_run_avb_coarse main.cpp ${BENCH_UTILS_SRC})
target_compile_definitions(bench_run_avb_coarse PRIVATE AVB_COARSE)
add_executable(hotset_gen generator/hotset_gen.cpp ${BENCH_UTILS_SRC})

if(ENABLE_SORTLEDTON)
    # SLT headers conflict with AVB headers, so it uses a separate executable.
    add_executable(bench_run_slt main.cpp ${BENCH_UTILS_SRC})
    target_compile_definitions(bench_run_slt PRIVATE USE_SLT ENABLE_SORTLEDTON)
endif()

if(ENABLE_GTX)
    target_compile_definitions(bench_run PRIVATE ENABLE_GTX)
    target_compile_definitions(bench_run_avb_coarse PRIVATE ENABLE_GTX)
endif()

if(ENABLE_GTX AND ENABLE_SORTLEDTON)
    target_compile_definitions(bench_run_slt PRIVATE ENABLE_GTX)
endif()

target_link_libraries(bench_run PRIVATE
    AVB_Core             # AVB
    neo_graph            # RapidStore
    livegraph            # LiveGraph shared library built from source.
    ${TESEO_LIB}         # Teseo static library built with autotools.
    event event_pthreads # Teseo uses libevent for rebalance and timer services.
    numa                 # Teseo uses libnuma for NUMA-aware allocation.
    neo_interface        # NeoGraph headers.
    lg_interface         # LiveGraph headers.
    teseo_interface      # Teseo headers, including generated static_configuration.hpp.
    OpenMP::OpenMP_CXX
    TBB::tbb
    Threads::Threads
    ZLIB::ZLIB
    ${CMAKE_DL_LIBS}
)

# coarse AVB variant link (target declared above at bench_run block)
target_link_libraries(bench_run_avb_coarse PRIVATE
    AVB_Core_coarse
    neo_graph            # RapidStore
    livegraph            # LiveGraph shared library built from source.
    ${TESEO_LIB}         # Teseo static library built with autotools.
    event event_pthreads # Teseo uses libevent for rebalance and timer services.
    numa                 # Teseo uses libnuma for NUMA-aware allocation.
    neo_interface        # NeoGraph headers.
    lg_interface         # LiveGraph headers.
    teseo_interface      # Teseo headers, including generated static_configuration.hpp.
    OpenMP::OpenMP_CXX
    TBB::tbb
    Threads::Threads
    ZLIB::ZLIB
    ${CMAKE_DL_LIBS}
)


target_link_libraries(hotset_gen PRIVATE
    OpenMP::OpenMP_CXX
    Threads::Threads
    ZLIB::ZLIB
)

if(ENABLE_GTX)
    target_link_libraries(bench_run PRIVATE gtx gtx_interface)
    target_link_libraries(bench_run_avb_coarse PRIVATE gtx gtx_interface)
endif()

if(ENABLE_SORTLEDTON)
    target_link_libraries(bench_run_slt PRIVATE
        sortledton slt_interface livegraph ${TESEO_LIB}
        event event_pthreads numa lg_interface teseo_interface
        OpenMP::OpenMP_CXX TBB::tbb Threads::Threads ZLIB::ZLIB ${CMAKE_DL_LIBS}
    )
    target_compile_options(bench_run_slt PRIVATE
        -Wno-deprecated-declarations -Wno-deprecated -g -fopenmp -O3 -std=c++17 -lpthread -ltbb -ltcmalloc -DJEMALLOC_NO_DEMANGLE
    )
    add_dependencies(bench_run_slt teseo_build)
endif()

if(ENABLE_GTX AND ENABLE_SORTLEDTON)
    target_link_libraries(bench_run_slt PRIVATE gtx gtx_interface)
endif()

# Build Teseo before linking the benchmark runners.
add_dependencies(bench_run teseo_build)
add_dependencies(bench_run_avb_coarse teseo_build)

target_compile_options(bench_run PRIVATE
    -Wno-deprecated-declarations -Wno-deprecated -g -fopenmp -O3 -std=c++17 -lpthread -ltbb -ltcmalloc -DJEMALLOC_NO_DEMANGLE
)
target_compile_options(bench_run_avb_coarse PRIVATE
    -Wno-deprecated-declarations -Wno-deprecated -g -fopenmp -O3 -std=c++17 -lpthread -ltbb -ltcmalloc -DJEMALLOC_NO_DEMANGLE
)
