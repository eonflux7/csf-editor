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
    src/csf_cmo.cpp
    src/csf_document.cpp
    src/csf_export.cpp
    src/csf_mission.cpp
    src/csf_mission_scene.cpp
    src/csf_object_database.cpp
    src/csf_overlay.cpp
)
add_library(csf::core ALIAS csf_core)
target_include_directories(csf_core PUBLIC include)
target_compile_features(csf_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(csf_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(csf_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(rws-info app/rws_info.cpp)
target_link_libraries(rws-info PRIVATE rws::core)

add_executable(rws-corpus app/rws_corpus.cpp)
target_link_libraries(rws-corpus PRIVATE rws::core)

add_executable(csf-info app/csf_info.cpp)
target_link_libraries(csf-info PRIVATE csf::core)

if(RWSMAN_BUILD_GUI)
    include(FetchContent)
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
        GIT_TAG v1.91.9b
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glfw imgui)

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

    add_executable(rws-man WIN32 app/main.cpp app/geometry_preview.cpp)
    target_link_libraries(rws-man PRIVATE rws::core csf::core imgui_backend)
    if(WIN32)
        target_link_libraries(rws-man PRIVATE Shell32 Comdlg32)
    endif()
endif()

if(RWSMAN_BUILD_TESTS)
    enable_testing()
    add_executable(rws_core_tests tests/document_tests.cpp)
    target_link_libraries(rws_core_tests PRIVATE rws::core csf::core)
    add_test(NAME rws_core_tests COMMAND rws_core_tests)
endif()
