cmake_minimum_required(VERSION 3.16)
project(zmq_location_server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR   ${CMAKE_SOURCE_DIR}/imgui)
set(IMPLOT_DIR  ${CMAKE_SOURCE_DIR}/implot)
set(GLFW_DIR    ${CMAKE_SOURCE_DIR}/glfw)
set(ZMQ_DIR     ${CMAKE_SOURCE_DIR}/zeromq)
set(JSON_DIR    ${CMAKE_SOURCE_DIR}/json)

find_package(OpenGL REQUIRED)

set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

add_subdirectory(${GLFW_DIR} glfw_build)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(ZMQ_PC libzmq QUIET IMPORTED_TARGET)
endif()

if(ZMQ_PC_FOUND)
    message(STATUS "Found system ZeroMQ: ${ZMQ_PC_LIBRARIES}")
    set(ZMQ_TARGET PkgConfig::ZMQ_PC)
else()
    message(STATUS "System ZeroMQ not found, building from submodule...")
    set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WITH_PERF_TOOL  OFF CACHE BOOL "" FORCE)
    set(WITH_DOC        OFF CACHE BOOL "" FORCE)
    set(ENABLE_DRAFTS   OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED    OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC    ON  CACHE BOOL "" FORCE)

    add_subdirectory(${ZMQ_DIR} zmq_build)

    if(TARGET libzmq-static)
        set(ZMQ_TARGET libzmq-static)
    elseif(TARGET zmq-static)
        set(ZMQ_TARGET zmq-static)
    elseif(TARGET libzmq)
        set(ZMQ_TARGET libzmq)
    else()
        message(FATAL_ERROR "Could not find ZeroMQ target")
    endif()
endif()

set(JSON_INCLUDE_DIR ${JSON_DIR}/include)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(imgui PUBLIC
    glfw
    OpenGL::GL
)

if(MSVC)
    target_compile_options(imgui PRIVATE /W0)
else()
    target_compile_options(imgui PRIVATE -w)
endif()

add_library(implot STATIC
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp
)

target_include_directories(implot PUBLIC
    ${IMPLOT_DIR}
    ${IMGUI_DIR}
)

target_link_libraries(implot PUBLIC imgui)

if(MSVC)
    target_compile_options(implot PRIVATE /W0)
else()
    target_compile_options(implot PRIVATE -w)
endif()

add_executable(zmq_server main.cpp)

target_include_directories(zmq_server PRIVATE
    ${JSON_INCLUDE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMPLOT_DIR}
)

if(ZMQ_PC_FOUND)
    target_include_directories(zmq_server PRIVATE ${ZMQ_PC_INCLUDE_DIRS})
    target_compile_options(zmq_server PRIVATE ${ZMQ_PC_CFLAGS_OTHER})
else()
    target_include_directories(zmq_server PRIVATE ${ZMQ_DIR}/include)
endif()

target_link_libraries(zmq_server PRIVATE
    imgui
    implot
    ${ZMQ_TARGET}
    OpenGL::GL
)

if(UNIX)
    target_link_libraries(zmq_server PRIVATE pthread ${CMAKE_DL_LIBS})
endif()

if(MSVC)
    target_compile_options(zmq_server PRIVATE /W3 /utf-8)
else()
    target_compile_options(zmq_server PRIVATE -Wall -Wextra -Wno-unused-variable)
endif()