cmake_minimum_required(VERSION 3.15)
project(FortniteESP)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ImGui source files
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../imgui-master")
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

# Include directories
include_directories(
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/../Ultra\ Detected\ Driver
)

# Main executable
add_executable(FortniteESP
    main.cpp
    ${IMGUI_SOURCES}
)

target_compile_definitions(FortniteESP PRIVATE _UNICODE UNICODE _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)

# Link libraries
target_link_libraries(FortniteESP
    d3d11
    psapi
    advapi32
)

# Set as Windows application (no console by default, but we allocate one in code)
set_target_properties(FortniteESP PROPERTIES
    WIN32_EXECUTABLE FALSE
)
