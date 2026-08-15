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
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    AU_SANDBOX_SAFE TRUE
    HARDENED_RUNTIME_ENABLED FALSE
    FORMATS AU VST3
    VST3_CATEGORIES Fx Analyzer
    PRODUCT_NAME "Audio Insight"
)

target_sources(AudioInsight
    PRIVATE
        src/analysis/HannSpectrumAnalyzer.cpp
        src/analysis/HannSpectrumAnalyzer.h
        src/analysis/StereoMeterAccumulator.cpp
        src/analysis/StereoMeterAccumulator.h
        src/analysis/StereoSampleCapture.cpp
        src/analysis/StereoSampleCapture.h
        src/core/VisualizationDataSource.h
        src/core/VisualizationFrame.h
        src/plugin/PluginProcessor.cpp
        src/plugin/PluginProcessor.h
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
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    src/analysis/HannSpectrumAnalyzer.cpp
    src/analysis/HannSpectrumAnalyzer.h
    src/analysis/StereoMeterAccumulator.cpp
    src/analysis/StereoMeterAccumulator.h
    src/analysis/StereoSampleCapture.cpp
    src/analysis/StereoSampleCapture.h
    src/core/VisualizationDataSource.h
    src/core/VisualizationFrame.h
    src/plugin/PluginProcessor.cpp
    src/plugin/PluginProcessor.h
)

if(BUILD_TESTING)
    juce_add_console_app(AudioInsightTests
        PRODUCT_NAME "Audio Insight Tests"
        NEEDS_CURL FALSE
        NEEDS_WEB_BROWSER FALSE
    )

    target_sources(AudioInsightTests
        PRIVATE
            src/analysis/HannSpectrumAnalyzer.cpp
            src/analysis/HannSpectrumAnalyzer.h
            src/analysis/StereoMeterAccumulator.cpp
            src/analysis/StereoMeterAccumulator.h
            src/analysis/StereoSampleCapture.cpp
            src/analysis/StereoSampleCapture.h
            src/core/VisualizationDataSource.h
            src/core/VisualizationFrame.h
            src/plugin/PluginProcessor.cpp
            src/plugin/PluginProcessor.h
            tests/AnalysisCoreTests.cpp
            tests/PluginProcessorTests.cpp
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
            juce::juce_audio_utils
            juce::juce_dsp
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )

    add_test(NAME AudioInsightTests COMMAND AudioInsightTests)

    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
        src/analysis/HannSpectrumAnalyzer.cpp
        src/analysis/HannSpectrumAnalyzer.h
        src/analysis/StereoMeterAccumulator.cpp
        src/analysis/StereoMeterAccumulator.h
        src/analysis/StereoSampleCapture.cpp
        src/analysis/StereoSampleCapture.h
        src/core/VisualizationDataSource.h
        src/core/VisualizationFrame.h
        src/plugin/PluginProcessor.cpp
        src/plugin/PluginProcessor.h
        tests/AnalysisCoreTests.cpp
        tests/PluginProcessorTests.cpp
        tests/TestMain.cpp
    )
endif()
