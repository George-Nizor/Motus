cmake_minimum_required(VERSION 3.25)

project(Motus VERSION 0.1.0 LANGUAGES CXX)

if(WIN32)
    enable_language(RC)
endif()

include(CTest)

option(MOTUS_BUILD_DESKTOP "Build the Motus Qt/QML desktop shell" ON)
option(MOTUS_BUILD_TESTS "Build unit tests" ON)
option(MOTUS_BUILD_MCP "Build the local Motus MCP server" ON)
option(MOTUS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

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
    src/mcp_server.cpp
    src/project_workflows.cpp
    src/media_probe.cpp
    src/media_integrity.cpp
    src/native_media.cpp
    src/process_runner.cpp
)
add_library(Motus::Core ALIAS ve_core)

target_include_directories(ve_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(ve_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(ve_core PRIVATE /W4 /permissive- /utf-8)
    if(MOTUS_WARNINGS_AS_ERRORS)
        target_compile_options(ve_core PRIVATE /WX)
    endif()
else()
    target_compile_options(ve_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(MOTUS_WARNINGS_AS_ERRORS)
        target_compile_options(ve_core PRIVATE -Werror)
    endif()
endif()

if(MOTUS_BUILD_MCP)
    add_executable(motus-mcp app/mcp_main.cpp)
    target_link_libraries(motus-mcp PRIVATE Motus::Core)
    install(TARGETS motus-mcp RUNTIME DESTINATION .)
endif()

if(MOTUS_BUILD_DESKTOP)
    find_package(Qt6 6.5 QUIET COMPONENTS Core Gui Multimedia Quick QuickControls2 Svg)
    if(Qt6_FOUND)
        qt_standard_project_setup(REQUIRES 6.5)
        qt_policy(SET QTP0004 NEW)
        qt_add_executable(motus
            app/main.cpp
            app/project_controller.cpp
            app/project_controller.h
            $<$<PLATFORM_ID:Windows>:${CMAKE_CURRENT_SOURCE_DIR}/app/motus.rc>
        )
        file(GLOB MOTUS_QML_ICON_ASSETS CONFIGURE_DEPENDS
            RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
            app/assets/icons/*.svg
        )
        qt_add_qml_module(motus
            URI Motus
            VERSION 1.0
            QML_FILES app/qml/Main.qml
            RESOURCES app/assets/motus-mark.svg ${MOTUS_QML_ICON_ASSETS}
        )
        target_link_libraries(motus PRIVATE
            Motus::Core Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Quick Qt6::QuickControls2 Qt6::Svg
        )
        target_compile_definitions(motus PRIVATE MOTUS_VERSION="${PROJECT_VERSION}")
        if(WIN32)
            set_target_properties(motus PROPERTIES WIN32_EXECUTABLE TRUE)
        endif()
        if(MOTUS_BUILD_TESTS)
            add_executable(motus_native_media_smoke
                app/native_media_smoke.cpp
                app/project_controller.cpp
                app/project_controller.h
            )
            target_link_libraries(motus_native_media_smoke PRIVATE Motus::Core Qt6::Core Qt6::Multimedia)
            add_test(NAME motus_native_media_smoke COMMAND motus_native_media_smoke)
            set_tests_properties(motus_native_media_smoke PROPERTIES TIMEOUT 90)
        endif()
        install(TARGETS motus RUNTIME DESTINATION .)
        configure_file(
            packaging/motus-bundle.json.in
            ${CMAKE_CURRENT_BINARY_DIR}/motus-bundle.json
            @ONLY
        )
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/motus-bundle.json DESTINATION .)
        if(WIN32)
            qt_generate_deploy_qml_app_script(
                TARGET motus
                OUTPUT_SCRIPT motus_deploy_script
                NO_UNSUPPORTED_PLATFORM_ERROR
            )
            install(SCRIPT ${motus_deploy_script})
        endif()
    else()
        message(STATUS "Qt 6.5+ not found; desktop shell will not be built")
    endif()
endif()

if(MOTUS_BUILD_TESTS)
    add_executable(ve_core_tests
        tests/test_main.cpp
        tests/media_time_tests.cpp
        tests/command_tests.cpp
        tests/cleanup_tests.cpp
        tests/persistence_tests.cpp
        tests/cache_tests.cpp
        tests/job_scheduler_tests.cpp
        tests/mlt_graph_tests.cpp
        tests/mcp_server_tests.cpp
        tests/media_probe_tests.cpp
        tests/media_integrity_tests.cpp
        tests/native_media_tests.cpp
    )
    target_link_libraries(ve_core_tests PRIVATE Motus::Core)
    add_test(NAME ve_core_tests COMMAND ve_core_tests)
endif()
