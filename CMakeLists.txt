cmake_minimum_required(VERSION 3.20)
project(Crate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# --- Dear ImGui (vendored, docking branch) ---------------------------------
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR})
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)

# --- Crate engine core -----------------------------------------------------
add_library(crate_core STATIC
    src/scene/Actor.cpp
    src/scene/Scene.cpp
    src/editor/Console.cpp
    src/editor/Theme.cpp
    src/editor/EditorApp.cpp
)
target_include_directories(crate_core PUBLIC src)
target_link_libraries(crate_core PUBLIC imgui)

# --- Editor executable ---------------------------------------------------
add_executable(Crate WIN32 src/main.cpp)
target_link_libraries(Crate PRIVATE crate_core)

if(MSVC)
    target_compile_options(crate_core PRIVATE /W4 /permissive-)
    target_compile_definitions(Crate PRIVATE UNICODE _UNICODE)
    # Keep the standard int main() entry point under the Windows subsystem.
    target_link_options(Crate PRIVATE /ENTRY:mainCRTStartup)
endif()

set_target_properties(Crate PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# --- Tests ---------------------------------------------------------------
enable_testing()
add_executable(actor_tests tests/actor_tests.cpp)
target_link_libraries(actor_tests PRIVATE crate_core)
add_test(NAME actor_tests COMMAND actor_tests)
