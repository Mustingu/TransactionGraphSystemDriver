cmake_minimum_required(VERSION 3.17)
project(AVBGraph)

# 1. Basic build settings.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 2. Find dependencies.
find_package(TBB REQUIRED)
find_package(OpenMP REQUIRED)
find_package(Threads REQUIRED)
find_library(TCMALLOC_LIB tcmalloc)

# =========================================================
# Recursively collect directories containing header files.
# =========================================================
macro(get_all_include_directories return_list current_dir)
    file(GLOB_RECURSE new_list LIST_DIRECTORIES true "${current_dir}/*.h" "${current_dir}/*.hpp")
    set(dir_list "")
    foreach(file_path ${new_list})
        get_filename_component(dir_path ${file_path} PATH)
        list(APPEND dir_list ${dir_path})
    endforeach()
    list(REMOVE_DUPLICATES dir_list)
    set(${return_list} ${dir_list})
endmacro()
# Collect all directories containing .h or .hpp files.
get_all_include_directories(ALL_INC_DIRS ${CMAKE_CURRENT_SOURCE_DIR})

# 3. Collect library sources, excluding standalone main functions.
file(GLOB_RECURSE ALGORITHM_SRC "algorithm/*.cpp")
file(GLOB_RECURSE DATA_STRUCT_SRC "data-structure/*.cpp")
file(GLOB_RECURSE UTILS_SRC "utils/*.cpp")
file(GLOB_RECURSE HAL_MEMORY_SRC "HALMemoryPool/*.cpp")
file(GLOB_RECURSE THIRD_PARTY_SRC "third-party/*.cpp")

set(AVB_SOURCES 
    ${ALGORITHM_SRC}
    ${DATA_STRUCT_SRC}
    ${UTILS_SRC}
    
    ${HAL_MEMORY_SRC}
    ${THIRD_PARTY_SRC}
    
)

# =========================================================
# Target A: build the AVB_Core static library.
# =========================================================
add_library(AVB_Core STATIC ${AVB_SOURCES})

# Include the collected header directories.
target_include_directories(AVB_Core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} # Include the source root.
    ${ALL_INC_DIRS}
)

# Link dependencies.
target_link_libraries(AVB_Core PUBLIC
    TBB::tbb
    OpenMP::OpenMP_CXX
    Threads::Threads
)

if(TCMALLOC_LIB)
    target_link_libraries(AVB_Core PUBLIC ${TCMALLOC_LIB})
endif()

# Compiler options.
target_compile_options(AVB_Core PRIVATE
     -Wno-deprecated-declarations -Wno-deprecated -g -fopenmp -O3 -std=c++17 -lpthread -ltbb -ltcmalloc -DJEMALLOC_NO_DEMANGLE
)

# =========================================================
# Epoch-snapshot library: AVB_COARSE disables FINEGRAIN in utils/utils.h.
# =========================================================
add_library(AVB_Core_coarse STATIC ${AVB_SOURCES})
target_include_directories(AVB_Core_coarse PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ALL_INC_DIRS}
)
target_compile_definitions(AVB_Core_coarse PUBLIC AVB_COARSE)
target_link_libraries(AVB_Core_coarse PUBLIC
    TBB::tbb
    OpenMP::OpenMP_CXX
    Threads::Threads
)
if(TCMALLOC_LIB)
    target_link_libraries(AVB_Core_coarse PUBLIC ${TCMALLOC_LIB})
endif()
target_compile_options(AVB_Core_coarse PRIVATE
     -Wno-deprecated-declarations -Wno-deprecated -g -fopenmp -O3 -std=c++17 -lpthread -ltbb -ltcmalloc -DJEMALLOC_NO_DEMANGLE
)
