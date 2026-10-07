cmake_minimum_required(VERSION 3.10)
project(wrapper)

find_package(OpenMP REQUIRED)
if(OpenMP_FOUND)
    message(STATUS "OpenMP found! Version: ${OpenMP_VERSION}")
    get_cmake_property(_variableNames VARIABLES)
    foreach (_variableName ${_variableNames})
        if (_variableName MATCHES "OpenMP")
            message(STATUS "${_variableName}=${${_variableName}}")
        endif()
    endforeach()
else()
    message(FATAL_ERROR "OpenMP not found!")
endif()

set(ITTNOTIFY_INCLUDE_DIR "/opt/intel/oneapi/vtune/latest/include/")
set(ITTNOTIFY_LIBRARY "/opt/intel/oneapi/vtune/latest/lib64/libittnotify.a")
include_directories(${ITTNOTIFY_INCLUDE_DIR})

# Enable TBB preview containers.
add_compile_options(-DTBB_PREVIEW_CONCURRENT_ORDERED_CONTAINERS=1)
add_compile_options( -Wall -O3 -g)

SET(LIBRARIES neo_wrapper.out)

ADD_EXECUTABLE(neo_wrapper.out wrapper.h apps/neo_wrapper/neo_wrapper.h apps/neo_wrapper/neo_wrapper.cpp)
TARGET_LINK_LIBRARIES(neo_wrapper.out PUBLIC neo_graph tbb ${OpenMP_CXX_LIBRARIES})

FOREACH(LIB IN LISTS LIBRARIES)
    # Link the dynamic-loader library.
    TARGET_LINK_LIBRARIES(${LIB} PUBLIC reader graph utils atomic ${OpenMP_CXX_LIBRARIES} ${ITTNOTIFY_LIBRARY} dl pthread)
    target_include_directories(${LIB} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(${LIB} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_include_directories(${LIB} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../libraries/NeoGraph)
    target_include_directories(${LIB} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../types)
ENDFOREACH ()
