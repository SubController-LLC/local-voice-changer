include(FetchContent)

set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

FetchContent_Declare(
  signalsmith_stretch
  URL https://github.com/Signalsmith-Audio/signalsmith-stretch/archive/57b93f4e9206a089a45387eaa39bdc9f310d3308.tar.gz
  URL_HASH SHA256=ad02e24334438b203e81d44f6c9906f3c6773e90a4ea923bb3e73d15697187d6
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR vvc-no-upstream-cmake)
FetchContent_MakeAvailable(signalsmith_stretch)

FetchContent_Declare(
  signalsmith_linear
  URL https://github.com/Signalsmith-Audio/linear/archive/refs/tags/0.3.1.tar.gz
  URL_HASH SHA256=b294471f1306baa4d968b230a8836924680e9ca068a667f4659259d66edbe9bc
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR vvc-no-upstream-cmake)
FetchContent_MakeAvailable(signalsmith_linear)

add_library(signalsmith_stretch INTERFACE)
target_include_directories(
  signalsmith_stretch SYSTEM
  INTERFACE "${signalsmith_stretch_SOURCE_DIR}" "${signalsmith_linear_SOURCE_DIR}/include")

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz
  URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

if(WIN32)
  FetchContent_Declare(
    miniaudio
    URL https://github.com/mackron/miniaudio/archive/refs/tags/0.11.25.tar.gz
    URL_HASH SHA256=b900edcffe979816e2560a0580b9b1216d674b4f17fbadeca8f777a7f8ab0274
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(miniaudio)

  add_library(miniaudio_headers INTERFACE)
  target_include_directories(miniaudio_headers SYSTEM INTERFACE "${miniaudio_SOURCE_DIR}")
endif()

if(VVC_BUILD_TESTS)
  FetchContent_Declare(
    Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.15.2.tar.gz
    URL_HASH SHA256=acfae120892c2b67a74142d36d060c0caa96f1c3aaa8aabd96e19961163d0420
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()
