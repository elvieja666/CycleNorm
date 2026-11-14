cmake_minimum_required(VERSION 3.22)
project(Toolkit VERSION 1.24.0)

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

juce_add_gui_app(Toolkit
    COMPANY_NAME "DIY"
    BUNDLE_ID com.diy.toolkit
    PRODUCT_NAME "Toolkit"
    DOCUMENT_EXTENSIONS wav aif aiff flac mp3 m4a caf
    VERSION ${PROJECT_VERSION})

# letra: Inter (OFL), en todo (también el título)
juce_add_binary_data(CycleNormFonts SOURCES
    Resources/Fonts/Inter-Light.otf
    Resources/Fonts/Inter-Regular.otf
    Resources/Fonts/Inter-Medium.otf
    Resources/Fonts/Inter-SemiBold.otf)

target_sources(Toolkit PRIVATE
    Source/Main.cpp
    Source/CycleEngine.h
    Source/ABSource.h
    Source/WaveformView.h
    Source/LoopFinder.h
    Source/Resample.h
    Source/Theme.h)

target_compile_definitions(Toolkit PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Toolkit,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Toolkit,JUCE_VERSION>")

target_link_libraries(Toolkit
    PRIVATE juce::juce_audio_utils
            CycleNormFonts
    PUBLIC juce::juce_recommended_config_flags
           juce::juce_recommended_lto_flags
           juce::juce_recommended_warning_flags)
