cmake_minimum_required(VERSION 3.22)

project(GenSynth VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE lives in a folder called "JUCE" next to this file
add_subdirectory(JUCE)

juce_add_plugin(GenSynth
    COMPANY_NAME "gen"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PLUGIN_MANUFACTURER_CODE Gnco
    PLUGIN_CODE Gsyn
    # Change to "AU VST3" if you also want a VST3 build
    FORMATS AU
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    PRODUCT_NAME "gen synth")

target_sources(GenSynth PRIVATE
    Source/PluginProcessor.cpp)

target_compile_definitions(GenSynth PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(GenSynth
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
