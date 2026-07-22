cmake_minimum_required(VERSION 3.28)

project(
  LocalVoiceChanger
  VERSION 0.3.0
  DESCRIPTION "Local realtime voice changer for Windows virtual microphone routing"
  LANGUAGES CXX)

option(VVC_BUILD_TESTS "Build automated tests" ON)
option(VVC_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)
include(CompilerWarnings)

add_library(
  vvc_core
  src/core/audio/SignalMetrics.cpp
  src/core/audio/WavFile.cpp
  src/core/dsp/FormantShifter.cpp
  src/core/dsp/SafetyChain.cpp
  src/core/dsp/VoiceProcessor.cpp
  src/realtime/AudioLifecycle.cpp
  src/realtime/DeviceRouting.cpp
  src/realtime/RealtimeAudioPipeline.cpp
  src/realtime/RealtimeVoiceProcessor.cpp)
add_library(vvc::core ALIAS vvc_core)

target_compile_features(vvc_core PUBLIC cxx_std_20)
target_include_directories(vvc_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vvc_core PUBLIC signalsmith_stretch nlohmann_json::nlohmann_json)
vvc_set_project_warnings(vvc_core)
vvc_enable_sanitizers(vvc_core)
if(MSVC)
  target_compile_options(vvc_core PRIVATE /guard:cf)
endif()

if(NOT MSVC)
  target_compile_options(vvc_core PRIVATE "$<$<CONFIG:Debug>:-O1>")
endif()

add_executable(vvc_bench src/tools/benchmark_main.cpp)
target_link_libraries(vvc_bench PRIVATE vvc::core)
target_compile_definitions(vvc_bench PRIVATE VVC_VERSION="${PROJECT_VERSION}")
vvc_set_project_warnings(vvc_bench)
vvc_enable_sanitizers(vvc_bench)

if(WIN32)
  enable_language(RC)
  add_executable(
    LocalVoiceChanger WIN32
    src/platform/windows/AppSettings.cpp
    src/platform/windows/AudioEngine.cpp
    src/platform/windows/MiniaudioImplementation.cpp
    src/platform/windows/SelfTest.cpp
    src/platform/windows/app_main.cpp
    src/platform/windows/app_resources.rc
    src/platform/windows/app.manifest)
  target_link_libraries(
    LocalVoiceChanger
    PRIVATE vvc::core miniaudio_headers avrt comctl32 dwmapi ole32 shell32 uuid)
  target_compile_definitions(
    LocalVoiceChanger PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(LocalVoiceChanger PRIVATE /guard:cf)
  target_link_options(
    LocalVoiceChanger PRIVATE /guard:cf /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT)
  set_source_files_properties(
    src/platform/windows/MiniaudioImplementation.cpp PROPERTIES COMPILE_OPTIONS "/W0")
  vvc_set_project_warnings(LocalVoiceChanger)
endif()

if(WIN32)
  install(TARGETS LocalVoiceChanger RUNTIME DESTINATION .)
endif()
install(FILES README.md LICENSE SECURITY.md THIRD_PARTY_NOTICES.md DESTINATION .)
install(DIRECTORY docs DESTINATION .)

if(VVC_BUILD_TESTS)
  include(CTest)
  enable_testing()

  add_executable(
    vvc_tests
    tests/test_formant_shifter.cpp
    tests/test_safety_chain.cpp
    tests/test_voice_processor.cpp
    tests/test_wav_file.cpp
    tests/realtime/test_audio_lifecycle.cpp
    tests/realtime/test_audio_ring_buffer.cpp
    tests/realtime/test_device_routing.cpp
    tests/realtime/test_realtime_audio_pipeline.cpp
    tests/realtime/test_realtime_voice_processor.cpp)
  target_link_libraries(vvc_tests PRIVATE vvc::core Catch2::Catch2WithMain)
  vvc_set_project_warnings(vvc_tests)
  vvc_enable_sanitizers(vvc_tests)

  add_executable(vvc_fixture tests/fixture_main.cpp)
  target_link_libraries(vvc_fixture PRIVATE vvc::core)
  vvc_set_project_warnings(vvc_fixture)
  vvc_enable_sanitizers(vvc_fixture)

  include(Catch)
  catch_discover_tests(vvc_tests)

  add_test(
    NAME cli.integration
    COMMAND
      "${CMAKE_COMMAND}"
      -DFIXTURE_EXE=$<TARGET_FILE:vvc_fixture>
      -DBENCH_EXE=$<TARGET_FILE:vvc_bench>
      -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-integration
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/RunCliIntegration.cmake")
endif()
