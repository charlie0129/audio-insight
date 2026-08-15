# SPDX-License-Identifier: AGPL-3.0-or-later

cmake_minimum_required(VERSION 3.25)

project(AudioInsight VERSION 0.1.0 LANGUAGES C CXX OBJCXX)

include(CTest)

if(NOT APPLE)
    message(FATAL_ERROR "Audio Insight currently supports the macOS development target only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Development architectures")

set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-")
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "YES")
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "YES")
set(CMAKE_XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME "NO")

set(
    AUDIO_INSIGHT_JUCE_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE"
    CACHE PATH
    "Path to the pinned JUCE source tree"
)

if(NOT EXISTS "${AUDIO_INSIGHT_JUCE_SOURCE_DIR}/CMakeLists.txt")
    message(
        FATAL_ERROR
        "JUCE was not found at '${AUDIO_INSIGHT_JUCE_SOURCE_DIR}'. "
        "Run 'git submodule update --init --recursive' or provide "
        "-DAUDIO_INSIGHT_JUCE_SOURCE_DIR=/absolute/path/to/JUCE."
    )
endif()

add_subdirectory("${AUDIO_INSIGHT_JUCE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")

find_library(AUDIO_INSIGHT_METAL_FRAMEWORK Metal REQUIRED)
find_library(AUDIO_INSIGHT_METALKIT_FRAMEWORK MetalKit REQUIRED)
find_library(AUDIO_INSIGHT_QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)

find_program(
    AUDIO_INSIGHT_CLANG_FORMAT_EXECUTABLE
    NAMES clang-format-22 clang-format
    DOC "clang-format executable used by the format targets"
)

set(AUDIO_INSIGHT_CLANG_FORMAT_COMPATIBLE FALSE)
set(
    AUDIO_INSIGHT_CLANG_FORMAT_ERROR
    "clang-format 22 was not found; install it or configure AUDIO_INSIGHT_CLANG_FORMAT_EXECUTABLE"
)

if(AUDIO_INSIGHT_CLANG_FORMAT_EXECUTABLE)
    execute_process(
        COMMAND "${AUDIO_INSIGHT_CLANG_FORMAT_EXECUTABLE}" --version
        RESULT_VARIABLE AUDIO_INSIGHT_CLANG_FORMAT_VERSION_RESULT
        OUTPUT_VARIABLE AUDIO_INSIGHT_CLANG_FORMAT_VERSION_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    if(AUDIO_INSIGHT_CLANG_FORMAT_VERSION_RESULT EQUAL 0)
        set(
            AUDIO_INSIGHT_CLANG_FORMAT_VERSION_PATTERN
            "clang-format version ([0-9]+)"
        )
        string(
            REGEX MATCH
            "${AUDIO_INSIGHT_CLANG_FORMAT_VERSION_PATTERN}"
            AUDIO_INSIGHT_CLANG_FORMAT_VERSION_MATCH
            "${AUDIO_INSIGHT_CLANG_FORMAT_VERSION_OUTPUT}"
        )

        if(AUDIO_INSIGHT_CLANG_FORMAT_VERSION_MATCH AND CMAKE_MATCH_1 STREQUAL "22")
            set(AUDIO_INSIGHT_CLANG_FORMAT_COMPATIBLE TRUE)
        else()
            set(
                AUDIO_INSIGHT_CLANG_FORMAT_ERROR
                "Audio Insight requires clang-format 22, but found: ${AUDIO_INSIGHT_CLANG_FORMAT_VERSION_OUTPUT}"
            )
        endif()
    endif()
endif()

file(
    GLOB_RECURSE AUDIO_INSIGHT_FORMAT_SOURCES
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES FALSE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hh"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hxx"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.m"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.metal"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cxx"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hh"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hxx"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.m"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.metal"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.mm"
)
list(SORT AUDIO_INSIGHT_FORMAT_SOURCES)

if(AUDIO_INSIGHT_CLANG_FORMAT_COMPATIBLE)
    add_custom_target(
        format
        COMMAND
            "${AUDIO_INSIGHT_CLANG_FORMAT_EXECUTABLE}"
            --style=file
            -i
            ${AUDIO_INSIGHT_FORMAT_SOURCES}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Formatting project-owned C++ and Objective-C++ sources"
        COMMAND_EXPAND_LISTS
        USES_TERMINAL
        VERBATIM
    )

    add_custom_target(
        format-check
        COMMAND
            "${AUDIO_INSIGHT_CLANG_FORMAT_EXECUTABLE}"
            --style=file
            --dry-run
            --Werror
            ${AUDIO_INSIGHT_FORMAT_SOURCES}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Checking project-owned C++ and Objective-C++ formatting"
        COMMAND_EXPAND_LISTS
        USES_TERMINAL
        VERBATIM
    )
else()
    add_custom_target(
        format
        COMMAND "${CMAKE_COMMAND}" -E echo "${AUDIO_INSIGHT_CLANG_FORMAT_ERROR}"
        COMMAND "${CMAKE_COMMAND}" -E false
        VERBATIM
    )

    add_custom_target(
        format-check
        COMMAND "${CMAKE_COMMAND}" -E echo "${AUDIO_INSIGHT_CLANG_FORMAT_ERROR}"
        COMMAND "${CMAKE_COMMAND}" -E false
        VERBATIM
    )
endif()

juce_add_plugin(AudioInsight
    VERSION "${PROJECT_VERSION}"
    COMPANY_NAME "charlie0129"
    COMPANY_WEBSITE "https://github.com/charlie0129/audio-insight"
    COMPANY_EMAIL "charlie_c_0129@outlook.com"
    BUNDLE_ID "io.github.charlie0129.audio-insight"
    PLUGIN_MANUFACTURER_CODE Chrl
    PLUGIN_CODE Ains
    DESCRIPTION "A responsive real-time audio visualization plugin"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    AU_SANDBOX_SAFE TRUE
    HARDENED_RUNTIME_ENABLED FALSE
    FORMATS AU VST3
    VST3_CATEGORIES Fx Analyzer
    PRODUCT_NAME "Audio Insight"
)

target_sources(AudioInsight
    PRIVATE
        src/analysis/AnalysisCoordinator.cpp
        src/analysis/AnalysisCoordinator.h
        src/analysis/HannSpectrumAnalyzer.cpp
        src/analysis/HannSpectrumAnalyzer.h
        src/analysis/SharedAnalysisScheduler.cpp
        src/analysis/SharedAnalysisScheduler.h
        src/analysis/StereoMeterAccumulator.cpp
        src/analysis/StereoMeterAccumulator.h
        src/analysis/StereoSampleCapture.cpp
        src/analysis/StereoSampleCapture.h
        src/core/VisualizationDataSource.h
        src/core/VisualizationFrame.h
        src/plugin/PluginEditor.cpp
        src/plugin/PluginEditor.h
        src/plugin/PluginProcessor.cpp
        src/plugin/PluginProcessor.h
        src/ui/MetalVisualization.h
        src/ui/MetalVisualization.mm
)

target_include_directories(AudioInsight PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(AudioInsight
    PUBLIC
        JUCE_MODAL_LOOPS_PERMITTED=0
        JUCE_REPORT_APP_USAGE=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_WEB_BROWSER=0
)

target_link_libraries(AudioInsight
    PRIVATE
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_gui_extra
        ${AUDIO_INSIGHT_METAL_FRAMEWORK}
        ${AUDIO_INSIGHT_METALKIT_FRAMEWORK}
        ${AUDIO_INSIGHT_QUARTZCORE_FRAMEWORK}
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    src/analysis/AnalysisCoordinator.cpp
    src/analysis/AnalysisCoordinator.h
    src/analysis/HannSpectrumAnalyzer.cpp
    src/analysis/HannSpectrumAnalyzer.h
    src/analysis/SharedAnalysisScheduler.cpp
    src/analysis/SharedAnalysisScheduler.h
    src/analysis/StereoMeterAccumulator.cpp
    src/analysis/StereoMeterAccumulator.h
    src/analysis/StereoSampleCapture.cpp
    src/analysis/StereoSampleCapture.h
    src/core/VisualizationDataSource.h
    src/core/VisualizationFrame.h
    src/plugin/PluginEditor.cpp
    src/plugin/PluginEditor.h
    src/plugin/PluginProcessor.cpp
    src/plugin/PluginProcessor.h
    src/ui/MetalVisualization.h
    src/ui/MetalVisualization.mm
)

if(BUILD_TESTING)
    juce_add_console_app(AudioInsightTests
        PRODUCT_NAME "Audio Insight Tests"
        NEEDS_CURL FALSE
        NEEDS_WEB_BROWSER FALSE
    )

    target_sources(AudioInsightTests
        PRIVATE
            src/analysis/AnalysisCoordinator.cpp
            src/analysis/AnalysisCoordinator.h
            src/analysis/HannSpectrumAnalyzer.cpp
            src/analysis/HannSpectrumAnalyzer.h
            src/analysis/SharedAnalysisScheduler.cpp
            src/analysis/SharedAnalysisScheduler.h
            src/analysis/StereoMeterAccumulator.cpp
            src/analysis/StereoMeterAccumulator.h
            src/analysis/StereoSampleCapture.cpp
            src/analysis/StereoSampleCapture.h
            src/core/VisualizationDataSource.h
            src/core/VisualizationFrame.h
            src/plugin/PluginEditor.cpp
            src/plugin/PluginEditor.h
            src/plugin/PluginProcessor.cpp
            src/plugin/PluginProcessor.h
            src/ui/MetalVisualization.h
            src/ui/MetalVisualization.mm
            tests/AnalysisCoordinatorTests.cpp
            tests/AnalysisCoreTests.cpp
            tests/PluginProcessorTests.cpp
            tests/SharedAnalysisSchedulerTests.cpp
            tests/TestMain.cpp
    )

    target_include_directories(AudioInsightTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_compile_definitions(AudioInsightTests
        PRIVATE
            JUCE_MODAL_LOOPS_PERMITTED=0
            JUCE_REPORT_APP_USAGE=0
            JUCE_UNIT_TESTS=1
            JUCE_USE_CURL=0
            JUCE_VST3_CAN_REPLACE_VST2=0
            JUCE_WEB_BROWSER=0
            JucePlugin_Name="Audio Insight"
    )

    target_link_libraries(AudioInsightTests
        PRIVATE
            juce::juce_audio_processors
            juce::juce_dsp
            juce::juce_gui_extra
            ${AUDIO_INSIGHT_METAL_FRAMEWORK}
            ${AUDIO_INSIGHT_METALKIT_FRAMEWORK}
            ${AUDIO_INSIGHT_QUARTZCORE_FRAMEWORK}
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )

    add_test(NAME AudioInsightTests COMMAND AudioInsightTests)

    add_test(
        NAME MetalDisplayLinkPresentationPolicy
        COMMAND
            "${CMAKE_COMMAND}"
            "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/src/ui/MetalVisualization.mm"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/CheckMetalDisplayLinkPresentation.cmake"
    )

    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
        src/analysis/AnalysisCoordinator.cpp
        src/analysis/AnalysisCoordinator.h
        src/analysis/HannSpectrumAnalyzer.cpp
        src/analysis/HannSpectrumAnalyzer.h
        src/analysis/SharedAnalysisScheduler.cpp
        src/analysis/SharedAnalysisScheduler.h
        src/analysis/StereoMeterAccumulator.cpp
        src/analysis/StereoMeterAccumulator.h
        src/analysis/StereoSampleCapture.cpp
        src/analysis/StereoSampleCapture.h
        src/core/VisualizationDataSource.h
        src/core/VisualizationFrame.h
        src/plugin/PluginEditor.cpp
        src/plugin/PluginEditor.h
        src/plugin/PluginProcessor.cpp
        src/plugin/PluginProcessor.h
        src/ui/MetalVisualization.h
        src/ui/MetalVisualization.mm
        tests/AnalysisCoordinatorTests.cpp
        tests/AnalysisCoreTests.cpp
        tests/PluginProcessorTests.cpp
        tests/SharedAnalysisSchedulerTests.cpp
        tests/TestMain.cpp
    )
endif()
