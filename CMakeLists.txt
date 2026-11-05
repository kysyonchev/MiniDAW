cmake_minimum_required(VERSION 3.22)
project(MiniDAW VERSION 1.2.2)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(FetchContent)
FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.4)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(MiniDAW
    PRODUCT_NAME "MiniDAW"
    BUNDLE_ID "com.minidaw.app"
    ICON_BIG   "${CMAKE_CURRENT_SOURCE_DIR}/icon/MiniDAW_icon_1024.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/icon/MiniDAW_icon_1024.png"
    DOCUMENT_EXTENSIONS mdp minidaw
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "MiniDAW needs audio input for your interface.")

target_sources(MiniDAW PRIVATE main.cpp Stemsplitter.h Stemsplit.h AudioCore.h)
target_compile_definitions(MiniDAW PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=1 JUCE_USE_MP3AUDIOFORMAT=1)
target_link_libraries(MiniDAW PRIVATE juce::juce_audio_utils juce::juce_gui_extra
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

add_custom_command(TARGET MiniDAW POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/icon/MiniDAW.icns"
        "$<TARGET_BUNDLE_CONTENT_DIR:MiniDAW>/Resources/Icon.icns")

option(MINIDAW_BUILD_TESTS "Build the AudioCore unit tests" OFF)
if(MINIDAW_BUILD_TESTS)
    add_executable(test_core test_core.cpp)
    target_include_directories(test_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()
