cmake_minimum_required(VERSION 3.22)
project(Node VERSION 1.37.0)

set(CMAKE_CXX_STANDARD 17)
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(Node
    COMPANY_NAME "DIY"
    BUNDLE_ID com.diy.node
    PRODUCT_NAME "Node"
    ICON_BIG ${CMAKE_CURRENT_SOURCE_DIR}/Resources/Icon/icon.png
    DOCUMENT_EXTENSIONS wav aif aiff flac mp3 m4a caf
    VERSION ${PROJECT_VERSION})

# letra: JetBrains Mono (OFL), en todo (también el título)
juce_add_binary_data(CycleNormFonts SOURCES
    Resources/Fonts/JetBrainsMonoNL-Light.ttf
    Resources/Fonts/JetBrainsMonoNL-Regular.ttf
    Resources/Fonts/JetBrainsMonoNL-Medium.ttf
    Resources/Fonts/JetBrainsMonoNL-SemiBold.ttf)

target_sources(Node PRIVATE
    Source/Main.cpp
    Source/CycleEngine.h
    Source/ABSource.h
    Source/WaveformView.h
    Source/LoopFinder.h
    Source/Resample.h
    Source/Theme.h)

target_compile_definitions(Node PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Node,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Node,JUCE_VERSION>")

target_link_libraries(Node
    PRIVATE juce::juce_audio_utils
            CycleNormFonts
    PUBLIC juce::juce_recommended_config_flags
           juce::juce_recommended_lto_flags
           juce::juce_recommended_warning_flags)
