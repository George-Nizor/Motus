cmake_minimum_required(VERSION 3.25)

project(VideoEditor VERSION 0.1.0 LANGUAGES CXX)

option(VE_BUILD_DESKTOP "Build the Qt/QML desktop shell" ON)
option(VE_BUILD_TESTS "Build unit tests" ON)
option(VE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(ve_core
    src/media_time.cpp
    src/project.cpp
    src/commands.cpp
    src/cleanup.cpp
    src/json.cpp
    src/project_store.cpp
    src/cache.cpp
    src/job_scheduler.cpp
    src/mlt_graph.cpp
)
add_library(VideoEditor::Core ALIAS ve_core)

target_include_directories(ve_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(ve_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(ve_core PRIVATE /W4 /permissive- /utf-8)
    if(VE_WARNINGS_AS_ERRORS)
        target_compile_options(ve_core PRIVATE /WX)
    endif()
else()
    target_compile_options(ve_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(VE_WARNINGS_AS_ERRORS)
        target_compile_options(ve_core PRIVATE -Werror)
    endif()
endif()

if(VE_BUILD_DESKTOP)
    find_package(Qt6 6.5 QUIET COMPONENTS Core Gui Quick QuickControls2)
    if(Qt6_FOUND)
        qt_standard_project_setup(REQUIRES 6.5)
        qt_add_executable(video-editor app/main.cpp)
        qt_add_qml_module(video-editor
            URI VideoEditor
            VERSION 1.0
            QML_FILES app/qml/Main.qml
        )
        target_link_libraries(video-editor PRIVATE
            VideoEditor::Core Qt6::Core Qt6::Gui Qt6::Quick Qt6::QuickControls2
        )
        install(TARGETS video-editor RUNTIME DESTINATION .)
    else()
        message(STATUS "Qt 6.5+ not found; desktop shell will not be built")
    endif()
endif()

if(VE_BUILD_TESTS)
    enable_testing()
    add_executable(ve_core_tests
        tests/test_main.cpp
        tests/media_time_tests.cpp
        tests/command_tests.cpp
        tests/cleanup_tests.cpp
        tests/persistence_tests.cpp
        tests/cache_tests.cpp
        tests/job_scheduler_tests.cpp
        tests/mlt_graph_tests.cpp
    )
    target_link_libraries(ve_core_tests PRIVATE VideoEditor::Core)
    add_test(NAME ve_core_tests COMMAND ve_core_tests)
endif()
