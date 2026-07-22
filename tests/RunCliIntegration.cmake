if(NOT DEFINED FIXTURE_EXE OR NOT DEFINED BENCH_EXE OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "CLI integration variables are missing")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

execute_process(
  COMMAND "${FIXTURE_EXE}" "${WORK_DIR}/input.wav"
  RESULT_VARIABLE fixture_result
  OUTPUT_VARIABLE fixture_output
  ERROR_VARIABLE fixture_error)
if(NOT fixture_result EQUAL 0)
  message(FATAL_ERROR "Fixture failed: ${fixture_output}${fixture_error}")
endif()

foreach(format pcm24 pcm32)
  execute_process(
    COMMAND "${FIXTURE_EXE}" "${WORK_DIR}/input-${format}.wav" "${format}"
    RESULT_VARIABLE extensible_fixture_result
    OUTPUT_VARIABLE extensible_fixture_output
    ERROR_VARIABLE extensible_fixture_error)
  if(NOT extensible_fixture_result EQUAL 0)
    message(
      FATAL_ERROR
        "${format} fixture failed: ${extensible_fixture_output}${extensible_fixture_error}")
  endif()

  execute_process(
    COMMAND "${BENCH_EXE}" --input "${WORK_DIR}/input-${format}.wav" --output-dir
            "${WORK_DIR}/output-${format}"
    RESULT_VARIABLE extensible_bench_result
    OUTPUT_VARIABLE extensible_bench_output
    ERROR_VARIABLE extensible_bench_error)
  if(NOT extensible_bench_result EQUAL 0)
    message(
      FATAL_ERROR
        "${format} vvc_bench failed: ${extensible_bench_output}${extensible_bench_error}")
  endif()
  foreach(expected dry.wav pitch-only.wav natural.wav metrics.json)
    if(NOT EXISTS "${WORK_DIR}/output-${format}/${expected}")
      message(FATAL_ERROR "Missing ${format} CLI artifact: ${expected}")
    endif()
  endforeach()
endforeach()

execute_process(
  COMMAND "${BENCH_EXE}" --input "${WORK_DIR}/input.wav" --output-dir "${WORK_DIR}/output"
          --pitch 3.0 --formant 2.0
  RESULT_VARIABLE bench_result
  OUTPUT_VARIABLE bench_output
  ERROR_VARIABLE bench_error)
if(NOT bench_result EQUAL 0)
  message(FATAL_ERROR "vvc_bench failed: ${bench_output}${bench_error}")
endif()

foreach(expected dry.wav pitch-only.wav natural.wav metrics.json)
  if(NOT EXISTS "${WORK_DIR}/output/${expected}")
    message(FATAL_ERROR "Missing CLI artifact: ${expected}")
  endif()
endforeach()

file(READ "${WORK_DIR}/output/metrics.json" metrics)
foreach(pattern
        "\"schema_version\": 1"
        "\"pitch_semitones\": 3"
        "\"formant_semitones\": 2"
        "\"network_used\": false"
        "\"non_finite_samples\": 0")
  if(NOT metrics MATCHES "${pattern}")
    message(FATAL_ERROR "metrics.json does not contain ${pattern}")
  endif()
endforeach()

execute_process(
  COMMAND "${BENCH_EXE}" --input "${WORK_DIR}/input.wav" --output-dir "${WORK_DIR}/invalid"
          --pitch invalid
  RESULT_VARIABLE invalid_result
  OUTPUT_VARIABLE invalid_output
  ERROR_VARIABLE invalid_error)
if(invalid_result EQUAL 0 OR NOT invalid_error MATCHES "エラー:")
  message(FATAL_ERROR "Invalid CLI input was not rejected in Japanese: ${invalid_output}${invalid_error}")
endif()

set(unicode_dir "${WORK_DIR}/日本語パス")
file(MAKE_DIRECTORY "${unicode_dir}")
file(COPY_FILE "${WORK_DIR}/input.wav" "${unicode_dir}/音声.wav")
execute_process(
  COMMAND "${BENCH_EXE}" --input "${unicode_dir}/音声.wav" --output-dir "${unicode_dir}/結果"
  RESULT_VARIABLE unicode_result
  OUTPUT_VARIABLE unicode_output
  ERROR_VARIABLE unicode_error)
if(NOT unicode_result EQUAL 0)
  message(FATAL_ERROR "Unicode path failed: ${unicode_output}${unicode_error}")
endif()
file(READ "${unicode_dir}/結果/metrics.json" unicode_metrics)
if(NOT unicode_metrics MATCHES "\"filename\": \"音声.wav\"")
  message(FATAL_ERROR "metrics.json did not preserve the UTF-8 filename: ${unicode_metrics}")
endif()

function(assert_collision_preserves_input input_path output_dir label)
  file(SHA256 "${input_path}" hash_before)
  execute_process(
    COMMAND "${BENCH_EXE}" --input "${input_path}" --output-dir "${output_dir}"
    RESULT_VARIABLE collision_result
    OUTPUT_VARIABLE collision_output
    ERROR_VARIABLE collision_error)
  if(collision_result EQUAL 0 OR NOT collision_error MATCHES "入力WAVと出力先が同じファイル")
    message(
      FATAL_ERROR
        "${label} collision was not safely rejected: ${collision_output}${collision_error}")
  endif()
  file(SHA256 "${input_path}" hash_after)
  if(NOT hash_before STREQUAL hash_after)
    message(FATAL_ERROR "${label} collision changed the input file")
  endif()
endfunction()

set(relative_collision_dir "${WORK_DIR}/relative-collision")
file(MAKE_DIRECTORY "${relative_collision_dir}")
file(COPY_FILE "${WORK_DIR}/input.wav" "${relative_collision_dir}/dry.wav")
file(SHA256 "${relative_collision_dir}/dry.wav" relative_hash_before)
execute_process(
  COMMAND "${BENCH_EXE}" --input dry.wav --output-dir .
  WORKING_DIRECTORY "${relative_collision_dir}"
  RESULT_VARIABLE relative_collision_result
  OUTPUT_VARIABLE relative_collision_output
  ERROR_VARIABLE relative_collision_error)
if(relative_collision_result EQUAL 0 OR
   NOT relative_collision_error MATCHES "入力WAVと出力先が同じファイル")
  message(
    FATAL_ERROR
      "Relative collision was not safely rejected: ${relative_collision_output}${relative_collision_error}")
endif()
file(SHA256 "${relative_collision_dir}/dry.wav" relative_hash_after)
if(NOT relative_hash_before STREQUAL relative_hash_after)
  message(FATAL_ERROR "Relative collision changed the input file")
endif()

set(temporary_sentinel_dir "${WORK_DIR}/temporary-sentinel")
file(MAKE_DIRECTORY "${temporary_sentinel_dir}")
foreach(sentinel dry.wav.tmp pitch-only.wav.tmp natural.wav.tmp metrics.json.tmp)
  file(WRITE "${temporary_sentinel_dir}/${sentinel}" "unrelated sentinel: ${sentinel}\n")
  file(SHA256 "${temporary_sentinel_dir}/${sentinel}" "${sentinel}_hash_before")
endforeach()
execute_process(
  COMMAND "${BENCH_EXE}" --input "${WORK_DIR}/input.wav" --output-dir
          "${temporary_sentinel_dir}"
  RESULT_VARIABLE sentinel_result
  OUTPUT_VARIABLE sentinel_output
  ERROR_VARIABLE sentinel_error)
if(NOT sentinel_result EQUAL 0)
  message(FATAL_ERROR "Fixed temporary sentinels blocked processing: ${sentinel_output}${sentinel_error}")
endif()
foreach(sentinel dry.wav.tmp pitch-only.wav.tmp natural.wav.tmp metrics.json.tmp)
  file(SHA256 "${temporary_sentinel_dir}/${sentinel}" "${sentinel}_hash_after")
  if(NOT "${${sentinel}_hash_before}" STREQUAL "${${sentinel}_hash_after}")
    message(FATAL_ERROR "Unrelated temporary sentinel was changed: ${sentinel}")
  endif()
endforeach()
file(GLOB staging_remnants "${temporary_sentinel_dir}/.vvc-stage-*")
if(staging_remnants)
  message(FATAL_ERROR "Staging directory was not cleaned: ${staging_remnants}")
endif()

set(hardlink_collision_dir "${WORK_DIR}/hardlink-collision")
file(MAKE_DIRECTORY "${hardlink_collision_dir}")
file(CREATE_LINK "${WORK_DIR}/input.wav" "${hardlink_collision_dir}/dry.wav"
     RESULT hardlink_result)
if(NOT hardlink_result STREQUAL "0")
  message(FATAL_ERROR "Could not create hard-link regression fixture: ${hardlink_result}")
endif()
assert_collision_preserves_input(
  "${WORK_DIR}/input.wav" "${hardlink_collision_dir}" "Hard-link")

set(symlink_collision_dir "${WORK_DIR}/symlink-collision")
file(MAKE_DIRECTORY "${symlink_collision_dir}")
file(CREATE_LINK "${WORK_DIR}/input.wav" "${symlink_collision_dir}/dry.wav"
     SYMBOLIC RESULT symlink_result)
if(symlink_result STREQUAL "0")
  assert_collision_preserves_input(
    "${WORK_DIR}/input.wav" "${symlink_collision_dir}" "Symbolic-link")
else()
  message(STATUS "Symbolic-link regression skipped on this host: ${symlink_result}")
endif()
