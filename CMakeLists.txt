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

# --- ImGuizmo (vendored) -----------------------------------------------
add_library(imguizmo STATIC third_party/imguizmo/ImGuizmo.cpp)
target_include_directories(imguizmo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imguizmo)
target_compile_definitions(imguizmo PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imguizmo PUBLIC imgui)
if(MSVC)
    target_compile_options(imguizmo PRIVATE /w)
endif()

# --- ufbx (vendored, single translation unit) -----------------------------
add_library(ufbx STATIC third_party/ufbx/ufbx.c)
target_include_directories(ufbx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
if(MSVC)
    target_compile_options(ufbx PRIVATE /w) # vendored code, silence warnings
endif()

# --- Shared, stateless script runtime ---------------------------------------
# Deliberately excludes anything that reaches an engine singleton
# (Console::get(), ComponentRegistry::get(), ScriptSystem::get(),
# AssetDatabase::get()) so it is safe to statically link into a future
# natively-compiled script DLL without creating a second, disconnected copy
# of those singletons (see transpiration.txt, "Transplation" Phase 0).
# Interpreter.cpp and ObjectDispatch.cpp joined this target in Phase 9a --
# both were already singleton-free (everything routes through the
# ScriptContext* they're handed), confirmed by re-reading their #include
# lists before moving them, so a script DLL can construct real Interpreter
# instances itself (needed so generated code can dispatch a cross-object
# call onto an INTERPRETED target without reimplementing the interpreter).
add_library(crate_script_runtime STATIC
    src/script/Value.cpp
    src/script/Runtime.cpp
    src/script/Interpreter.cpp
    src/script/ObjectDispatch.cpp
    src/scene/Actor.cpp
)
target_include_directories(crate_script_runtime PUBLIC src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
if(MSVC)
    target_compile_options(crate_script_runtime PRIVATE /W4 /permissive-)
endif()

# --- Crate engine core -----------------------------------------------------
add_library(crate_core STATIC
    src/core/Log.cpp
    src/scene/Scene.cpp
    src/scene/SceneIO.cpp
    src/scene/ComponentRegistry.cpp
    src/scene/BuiltinComponents.cpp
    src/script/Lexer.cpp
    src/script/Parser.cpp
    src/script/CodeGen.cpp
    src/script/NativeModule.cpp
    src/script/NativeClassRegistry.cpp
    src/script/NativeScriptComponent.cpp
    src/script/Format.cpp
    src/script/ScriptComponent.cpp
    src/script/ScriptFieldIO.cpp
    src/script/ScriptSystem.cpp
    src/render/Mesh.cpp
    src/render/Renderer.cpp
    src/assets/AssetDatabase.cpp
    src/assets/FbxImport.cpp
    src/assets/Image.cpp
    src/input/InputMap.cpp
    src/input/Input.cpp
    src/platform/FileDialog.cpp
    src/editor/AssetPicker.cpp
    src/editor/Console.cpp
    src/editor/ContextMenu.cpp
    src/editor/Popup.cpp
    src/editor/ScriptBuild.cpp
    src/editor/ScriptEditor.cpp
    src/editor/Theme.cpp
    src/editor/EditorApp.cpp
)
target_include_directories(crate_core PUBLIC src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(crate_core PUBLIC crate_script_runtime imgui imgui_texteditor imguizmo ufbx comdlg32 xinput)
# For ScriptBuild.cpp (Phase 3): where to -I from when compiling a generated
# script DLL, and where crate_script_runtime.lib for the RUNNING config lives
# (CMake's default static-lib output directory, i.e. this build tree's root).
target_compile_definitions(crate_core PRIVATE
    CRATE_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    CRATE_ENGINE_BUILD_DIR="${CMAKE_BINARY_DIR}")

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

add_executable(runtime_tests tests/runtime_tests.cpp)
target_link_libraries(runtime_tests PRIVATE crate_core)
add_test(NAME runtime_tests COMMAND runtime_tests)

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

add_executable(asset_db_tests tests/asset_db_tests.cpp)
target_link_libraries(asset_db_tests PRIVATE crate_core)
add_test(NAME asset_db_tests COMMAND asset_db_tests)

add_executable(lighting_bake_tests tests/lighting_bake_tests.cpp)
target_link_libraries(lighting_bake_tests PRIVATE crate_core)
add_test(NAME lighting_bake_tests COMMAND lighting_bake_tests)

add_executable(script_bug_tests tests/script_bug_tests.cpp)
target_link_libraries(script_bug_tests PRIVATE crate_core)
add_test(NAME script_bug_tests COMMAND script_bug_tests)

add_executable(script_tests tests/script_tests.cpp)
target_link_libraries(script_tests PRIVATE crate_core)
add_test(NAME script_tests COMMAND script_tests)

add_executable(script_integration_tests tests/script_integration_tests.cpp)
target_link_libraries(script_integration_tests PRIVATE crate_core)
target_compile_definitions(script_integration_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME script_integration_tests COMMAND script_integration_tests)

add_executable(script_metadata_tests tests/script_metadata_tests.cpp)
target_link_libraries(script_metadata_tests PRIVATE crate_core)
target_compile_definitions(script_metadata_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME script_metadata_tests COMMAND script_metadata_tests)

add_executable(codegen_tests tests/codegen_tests.cpp)
target_link_libraries(codegen_tests PRIVATE crate_core)
target_compile_definitions(codegen_tests PRIVATE
    CRATE_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME codegen_tests COMMAND codegen_tests)

add_executable(script_build_tests tests/script_build_tests.cpp)
target_link_libraries(script_build_tests PRIVATE crate_core)
target_compile_definitions(script_build_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME script_build_tests COMMAND script_build_tests)

add_executable(native_module_tests tests/native_module_tests.cpp)
target_link_libraries(native_module_tests PRIVATE crate_core)
target_compile_definitions(native_module_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME native_module_tests COMMAND native_module_tests)

add_executable(native_reflection_tests tests/native_reflection_tests.cpp)
target_link_libraries(native_reflection_tests PRIVATE crate_core)
target_compile_definitions(native_reflection_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME native_reflection_tests COMMAND native_reflection_tests)

add_executable(do_async_native_tests tests/do_async_native_tests.cpp)
target_link_libraries(do_async_native_tests PRIVATE crate_core)
target_compile_definitions(do_async_native_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME do_async_native_tests COMMAND do_async_native_tests)

add_executable(object_dispatch_tests tests/object_dispatch_tests.cpp)
target_link_libraries(object_dispatch_tests PRIVATE crate_core)
target_compile_definitions(object_dispatch_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME object_dispatch_tests COMMAND object_dispatch_tests)

add_executable(get_component_parity_tests tests/get_component_parity_tests.cpp)
target_link_libraries(get_component_parity_tests PRIVATE crate_core)
target_compile_definitions(get_component_parity_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME get_component_parity_tests COMMAND get_component_parity_tests)

add_executable(camera_main_parity_tests tests/camera_main_parity_tests.cpp)
target_link_libraries(camera_main_parity_tests PRIVATE crate_core)
target_compile_definitions(camera_main_parity_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME camera_main_parity_tests COMMAND camera_main_parity_tests)

add_executable(input_mouse_parity_tests tests/input_mouse_parity_tests.cpp)
target_link_libraries(input_mouse_parity_tests PRIVATE crate_core)
target_compile_definitions(input_mouse_parity_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME input_mouse_parity_tests COMMAND input_mouse_parity_tests)

add_executable(signals_parity_tests tests/signals_parity_tests.cpp)
target_link_libraries(signals_parity_tests PRIVATE crate_core)
target_compile_definitions(signals_parity_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME signals_parity_tests COMMAND signals_parity_tests)

add_executable(static_class_parity_tests tests/static_class_parity_tests.cpp)
target_link_libraries(static_class_parity_tests PRIVATE crate_core)
target_compile_definitions(static_class_parity_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME static_class_parity_tests COMMAND static_class_parity_tests)

add_executable(inheritance_parity_tests tests/inheritance_parity_tests.cpp)
target_link_libraries(inheritance_parity_tests PRIVATE crate_core)
target_compile_definitions(inheritance_parity_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME inheritance_parity_tests COMMAND inheritance_parity_tests)

add_executable(cross_namespace_inheritance_tests tests/cross_namespace_inheritance_tests.cpp)
target_link_libraries(cross_namespace_inheritance_tests PRIVATE crate_core)
target_compile_definitions(cross_namespace_inheritance_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME cross_namespace_inheritance_tests COMMAND cross_namespace_inheritance_tests)

add_executable(scene_io_tests tests/scene_io_tests.cpp)
target_link_libraries(scene_io_tests PRIVATE crate_core)
target_compile_definitions(scene_io_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME scene_io_tests COMMAND scene_io_tests)

add_executable(scene_instancing_tests tests/scene_instancing_tests.cpp)
target_link_libraries(scene_instancing_tests PRIVATE crate_core)
add_test(NAME scene_instancing_tests COMMAND scene_instancing_tests)

add_executable(scene_instantiate_script_tests tests/scene_instantiate_script_tests.cpp)
target_link_libraries(scene_instantiate_script_tests PRIVATE crate_core)
target_compile_definitions(scene_instantiate_script_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME scene_instantiate_script_tests COMMAND scene_instantiate_script_tests)

add_executable(actor_api_tests tests/actor_api_tests.cpp)
target_link_libraries(actor_api_tests PRIVATE crate_core)
target_compile_definitions(actor_api_tests PRIVATE
    CRATE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts")
add_test(NAME actor_api_tests COMMAND actor_api_tests)
