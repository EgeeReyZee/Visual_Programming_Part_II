cmake_minimum_required(VERSION 3.16)
project(zmq_location_server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ================================================================
#  Пути к сабмодулям (все лежат в корне репозитория)
# ================================================================
set(IMGUI_DIR   ${CMAKE_SOURCE_DIR}/imgui)
set(GLFW_DIR    ${CMAKE_SOURCE_DIR}/glfw)
set(ZMQ_DIR     ${CMAKE_SOURCE_DIR}/zeromq)
set(JSON_DIR    ${CMAKE_SOURCE_DIR}/json)

# ================================================================
#  OpenGL  (системный)
# ================================================================
find_package(OpenGL REQUIRED)

# ================================================================
#  GLFW  — собирается из сабмодуля
# ================================================================
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

add_subdirectory(${GLFW_DIR} glfw_build)

# ================================================================
#  ZeroMQ — ищем системный или используем сабмодуль
# ================================================================
# Сначала пытаемся найти системный ZeroMQ
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ_PC libzmq QUIET IMPORTED_TARGET)

if(ZMQ_PC_FOUND)
    message(STATUS "Found system ZeroMQ: ${ZMQ_PC_LIBRARIES}")
    set(ZMQ_TARGET PkgConfig::ZMQ_PC)
else()
    message(STATUS "System ZeroMQ not found, building from submodule...")
    # Собираем из сабмодуля
    set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WITH_PERF_TOOL  OFF CACHE BOOL "" FORCE)
    set(WITH_DOC        OFF CACHE BOOL "" FORCE)
    set(ENABLE_DRAFTS   OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED    OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC    ON  CACHE BOOL "" FORCE)
    
    add_subdirectory(${ZMQ_DIR} zmq_build)
    
    # Проверяем имя цели
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

# ================================================================
#  nlohmann/json  — header-only
# ================================================================
set(JSON_INCLUDE_DIR ${JSON_DIR}/include)

# ================================================================
#  ImGui  — нет своего CMakeLists, собираем вручную
# ================================================================
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

# ================================================================
#  Основной исполняемый файл
# ================================================================
add_executable(zmq_server
    main.cpp
)

target_include_directories(zmq_server PRIVATE
    ${JSON_INCLUDE_DIR}
)

# Добавляем include директории для ZeroMQ
if(ZMQ_PC_FOUND)
    # Для системного ZeroMQ
    target_include_directories(zmq_server PRIVATE ${ZMQ_PC_INCLUDE_DIRS})
    target_compile_options(zmq_server PRIVATE ${ZMQ_PC_CFLAGS_OTHER})
else()
    # Для собранного из сабмодуля
    target_include_directories(zmq_server PRIVATE ${ZMQ_DIR}/include)
endif()

target_link_libraries(zmq_server PRIVATE
    imgui
    ${ZMQ_TARGET}
    OpenGL::GL
)

if(UNIX)
    target_link_libraries(zmq_server PRIVATE pthread ${CMAKE_DL_LIBS})
endif()

# ================================================================
#  Флаги компилятора
# ================================================================
if(MSVC)
    target_compile_options(zmq_server PRIVATE /W3 /utf-8)
    target_compile_options(imgui      PRIVATE /W0)
else()
    target_compile_options(zmq_server PRIVATE -Wall -Wextra -Wno-unused-variable)
endif()