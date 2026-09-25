cmake_minimum_required(VERSION 3.24)

project(csf-rws-tools VERSION 0.1.0 LANGUAGES CXX)

option(RWSMAN_BUILD_GUI "Build the Dear ImGui desktop application" ON)
option(RWSMAN_BUILD_TESTS "Build parser tests" ON)

add_library(rws_core
    src/animation.cpp
    src/document.cpp
    src/chunk_names.cpp
    src/decoded.cpp
    src/physics_inspection.cpp
    src/world_recovery.cpp
    src/world_model.cpp
    src/world_source.cpp
    src/world_queries.cpp
    src/map_assembly.cpp
    src/obj_export.cpp
    src/scene_export.cpp
)
add_library(rws::core ALIAS rws_core)
target_include_directories(rws_core PUBLIC include)
target_compile_features(rws_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(rws_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(rws_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(csf_core
    src/csf_animation_catalog.cpp
    src/csf_animation_slots.cpp
    src/csf_authoring.cpp
    src/csf_authoring_project.cpp
    src/csf_cmo.cpp
    src/csf_document.cpp
    src/csf_export.cpp
    src/csf_mission.cpp
    src/csf_mission_edit.cpp
    src/csf_mission_flow.cpp
    src/csf_mission_ops.cpp
    src/csf_mission_recipes.cpp
    src/csf_mission_scene.cpp
    src/csf_mod_project.cpp
    src/csf_object_database.cpp
    src/csf_overlay.cpp
    src/csf_program.cpp
    src/csf_script_check.cpp
    src/csf_script_signatures.cpp
    src/csf_source_text.cpp
    src/csf_tree.cpp
)
add_library(csf::core ALIAS csf_core)
target_include_directories(csf_core PUBLIC include)
# Authoring projects compile Worlds and sector maps (rws_core).
target_link_libraries(csf_core PUBLIC rws_core)
target_compile_features(csf_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(csf_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(csf_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Mission PAK export links pak-man's archive library when its source tree is
# available (by default the sibling ../pak-man checkout). Without it, csf-mod
# and the GUI run an external pakman-cli instead.
set(RWSMAN_PAKMAN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pak-man" CACHE PATH
    "pak-man source tree linked for built-in mission PAK export")
if(EXISTS "${RWSMAN_PAKMAN_DIR}/CMakeLists.txt")
    set(PAKMAN_CORE_ONLY ON CACHE BOOL "" FORCE)
    add_subdirectory("${RWSMAN_PAKMAN_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/pakman" EXCLUDE_FROM_ALL)
    target_link_libraries(csf_core PRIVATE pakman_core)
    target_compile_definitions(csf_core PRIVATE RWSMAN_HAVE_PAKMAN=1)
    message(STATUS "Mission PAK export: built in (${RWSMAN_PAKMAN_DIR})")
else()
    message(STATUS "Mission PAK export: external pakman-cli (pak-man source not found)")
endif()

# GUI-free helpers behind the workbench: settings, command registry, fuzzy
# search, navigation history, the operation log, and symbol indexes. It has no
# ImGui, GLFW, or OpenGL dependency, so the core-only build and the tests cover it.
add_library(rwsman_ui_model STATIC
    src/ui_model_chunks.cpp
    src/ui_model_commands.cpp
    src/ui_model_diagnostics.cpp
    src/ui_model_discovery.cpp
    src/ui_model_frame_pacing.cpp
    src/ui_model_fuzzy.cpp
    src/ui_model_history.cpp
    src/ui_model_index_builders.cpp
    src/ui_model_log.cpp
    src/ui_model_mission_lookup.cpp
    src/ui_model_search.cpp
    src/ui_model_settings.cpp
    src/ui_model_viewport_overlays.cpp
)
add_library(rwsman::ui_model ALIAS rwsman_ui_model)
target_include_directories(rwsman_ui_model PUBLIC include)
target_compile_features(rwsman_ui_model PUBLIC cxx_std_20)
target_link_libraries(rwsman_ui_model PUBLIC rws::core csf::core)
if(MSVC)
    target_compile_options(rwsman_ui_model PRIVATE /W4 /permissive-)
else()
    target_compile_options(rwsman_ui_model PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(FetchContent)

# stb_image is a portable, single-header PNG decoder shared by the GUI and the
# tests. The upstream repository publishes no release tags, so pin an exact
# commit for reproducible fetches.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
)
FetchContent_MakeAvailable(stb)

add_library(stb_image STATIC src/stb_image_impl.cpp src/stb_image_write_impl.cpp)
target_include_directories(stb_image PUBLIC ${stb_SOURCE_DIR})

add_library(rws_image STATIC src/texture_image.cpp)
add_library(rws::image ALIAS rws_image)
target_include_directories(rws_image PUBLIC include)
target_compile_features(rws_image PUBLIC cxx_std_20)
target_link_libraries(rws_image PUBLIC stb_image)
if(MSVC)
    target_compile_options(rws_image PRIVATE /W4 /permissive-)
else()
    target_compile_options(rws_image PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(rws-info app/rws_info.cpp)
target_link_libraries(rws-info PRIVATE rws::core)

add_executable(rws-corpus app/rws_corpus.cpp)
target_link_libraries(rws-corpus PRIVATE rws::core)

add_executable(csf-info app/csf_info.cpp)
target_link_libraries(csf-info PRIVATE csf::core)

add_executable(csf-mod app/csf_mod.cpp)
target_link_libraries(csf-mod PRIVATE csf::core rws::core)

if(RWSMAN_BUILD_GUI)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE
    )
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.9b-docking
        GIT_SHALLOW TRUE
    )
    # portable-file-dialogs: single-header native file dialogs (zenity/kdialog on
    # Linux, Win32/COM on Windows). Pinned to an exact commit; it publishes no tags.
    FetchContent_Declare(pfd
        GIT_REPOSITORY https://github.com/samhocevar/portable-file-dialogs.git
        GIT_TAG c12ea8c9a727f5320a2b4570aee863bbede2a204
        # Only the header is used; skip the project's own (very old) CMakeLists.
        SOURCE_SUBDIR header-only
    )
    FetchContent_MakeAvailable(glfw imgui pfd)

    find_package(OpenGL REQUIRED)
    add_library(imgui_backend STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui_backend PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(imgui_backend PUBLIC glfw OpenGL::GL)
    target_compile_features(imgui_backend PUBLIC cxx_std_20)

    add_executable(rws-man WIN32
        app/main.cpp
        app/app_actions.cpp
        app/commands.cpp
        app/file_dialogs.cpp
        app/geometry_preview.cpp
        app/geometry_preview_editing.cpp
        app/geometry_preview_overlays.cpp
        app/mission_editing.cpp
        app/authoring.cpp
        app/mission_authoring.cpp
        app/mission_loader.cpp
        app/mission_overlays.cpp
        app/navigation.cpp
        app/references.cpp
        app/screenshot.cpp
        app/ui/actor_animation.cpp
        app/ui/center.cpp
        app/ui/chunk_details.cpp
        app/ui/console.cpp
        app/ui/diagnostics.cpp
        app/ui/explorer.cpp
        app/ui/fonts.cpp
        app/ui/hex_view.cpp
        app/ui/inspector.cpp
        app/ui/layout.cpp
        app/ui/menus.cpp
        app/ui/property_grid.cpp
        app/ui/mission_details.cpp
        app/ui/mission_editor.cpp
        app/ui/overlays.cpp
        app/ui/preferences.cpp
        app/ui/height_report.cpp
        app/ui/palette.cpp
        app/ui/panels.cpp
        app/ui/script_view.cpp
        app/ui/shell.cpp
        app/ui/shortcuts.cpp
        app/ui/start_page.cpp
        app/ui/status_bar.cpp
        app/ui/theme.cpp
        app/ui/widgets.cpp
    )
    target_include_directories(rws-man PRIVATE app ${pfd_SOURCE_DIR})
    target_compile_definitions(rws-man PRIVATE RWSMAN_VERSION="${PROJECT_VERSION}")
    target_link_libraries(rws-man PRIVATE rws::core csf::core rws::image rwsman::ui_model imgui_backend)
    if(WIN32)
        target_link_libraries(rws-man PRIVATE Shell32 Comdlg32)
    endif()
endif()

if(RWSMAN_BUILD_TESTS)
    enable_testing()
    add_executable(rws_core_tests tests/document_tests.cpp)
    target_link_libraries(rws_core_tests PRIVATE rws::core csf::core rws::image rwsman::ui_model)
    add_test(NAME rws_core_tests COMMAND rws_core_tests)
    add_executable(csf_authoring_tests tests/authoring_tests.cpp)
    target_link_libraries(csf_authoring_tests PRIVATE csf::core)
    add_executable(fake_pakman tests/fake_pakman.cpp)
    target_compile_features(fake_pakman PRIVATE cxx_std_20)
    add_test(NAME csf_authoring_tests COMMAND csf_authoring_tests $<TARGET_FILE:fake_pakman>)
endif()
