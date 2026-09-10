cmake_minimum_required(VERSION 3.20)
project(Crate LANGUAGES CXX C)

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

# --- ImGuiColorTextEdit (vendored) ---------------------------------------
add_library(imgui_texteditor STATIC third_party/imgui_texteditor/TextEditor.cpp)
target_include_directories(imgui_texteditor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui_texteditor)
target_link_libraries(imgui_texteditor PUBLIC imgui)
if(MSVC)
    target_compile_options(imgui_texteditor PRIVATE /w)
endif()

# --- ufbx (vendored, single translation unit) -----------------------------
add_library(ufbx STATIC third_party/ufbx/ufbx.c)
target_include_directories(ufbx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
if(MSVC)
    target_compile_options(ufbx PRIVATE /w) # vendored code, silence warnings
endif()

# --- Crate engine core -----------------------------------------------------
add_library(crate_core STATIC
    src/core/Log.cpp
    src/scene/Actor.cpp
    src/scene/Scene.cpp
    src/scene/ComponentRegistry.cpp
    src/scene/BuiltinComponents.cpp
    src/script/Lexer.cpp
    src/script/Parser.cpp
    src/script/Value.cpp
    src/script/Interpreter.cpp
    src/script/Format.cpp
    src/script/ScriptComponent.cpp
    src/script/ScriptSystem.cpp
    src/render/Mesh.cpp
    src/render/Renderer.cpp
    src/assets/FbxImport.cpp
    src/assets/Image.cpp
    src/platform/FileDialog.cpp
    src/editor/Console.cpp
    src/editor/ContextMenu.cpp
    src/editor/Popup.cpp
    src/editor/ScriptEditor.cpp
    src/editor/Theme.cpp
    src/editor/EditorApp.cpp
)
target_include_directories(crate_core PUBLIC src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(crate_core PUBLIC imgui imgui_texteditor ufbx comdlg32)

# --- Editor executable ---------------------------------------------------
add_executable(Crate WIN32 src/main.cpp)
target_link_libraries(Crate PRIVATE crate_core shell32)

if(MSVC)
    target_compile_options(crate_core PRIVATE /W4 /permissive-)
    target_compile_definitions(Crate PRIVATE UNICODE _UNICODE)
    # Keep the standard int main() entry point under the Windows subsystem.
    target_link_options(Crate PRIVATE /ENTRY:mainCRTStartup)
    # Non-incremental links: fast enough here and avoids intermittent
    # LNK1163 (stale COMDAT) after header changes.
    add_link_options(/INCREMENTAL:NO)
endif()

set_target_properties(Crate PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Copy bundled assets next to the executable so a launched build finds them.
add_custom_command(TARGET Crate POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:Crate>/assets)

# --- Tests ---------------------------------------------------------------
enable_testing()
add_executable(actor_tests tests/actor_tests.cpp)
target_link_libraries(actor_tests PRIVATE crate_core)
add_test(NAME actor_tests COMMAND actor_tests)

add_executable(fbx_tests tests/fbx_tests.cpp)
target_link_libraries(fbx_tests PRIVATE crate_core)
target_compile_definitions(fbx_tests PRIVATE
    CRATE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
add_test(NAME fbx_tests COMMAND fbx_tests)

add_executable(image_tests tests/image_tests.cpp)
target_link_libraries(image_tests PRIVATE crate_core)
target_compile_definitions(image_tests PRIVATE
    CRATE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
add_test(NAME image_tests COMMAND image_tests)

add_executable(mesh_tests tests/mesh_tests.cpp)
target_link_libraries(mesh_tests PRIVATE crate_core)
add_test(NAME mesh_tests COMMAND mesh_tests)

add_executable(script_tests tests/script_tests.cpp)
target_link_libraries(script_tests PRIVATE crate_core)
add_test(NAME script_tests COMMAND script_tests)

add_executable(script_integration_tests tests/script_integration_tests.cpp)
target_link_libraries(script_integration_tests PRIVATE crate_core)
target_compile_definitions(script_integration_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME script_integration_tests COMMAND script_integration_tests)
