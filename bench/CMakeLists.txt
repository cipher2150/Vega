# Each benchmark gets the shared main, which places the main thread like the production
# book thread before running (QoS on macOS, optional pinning on Linux).
add_library(cb_bench_main STATIC bench_main.cpp)
target_link_libraries(cb_bench_main PUBLIC cb_core cb_platform benchmark::benchmark)

foreach(name bench_spsc bench_decimal bench_book bench_pipeline_synthetic bench_checksum)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE cb_bench_main)
  cb_set_warnings(${name})
endforeach()

# Real-data pipeline benchmark: needs the engine library and the committed recording.
add_executable(bench_pipeline_replay bench_pipeline_replay.cpp)
target_link_libraries(bench_pipeline_replay PRIVATE cb_bench_main cb_lib)
target_compile_definitions(bench_pipeline_replay PRIVATE
  CB_GOLDEN_RECORDING="${PROJECT_SOURCE_DIR}/tests/data/golden_session.cbrec.gz")
cb_set_warnings(bench_pipeline_replay)
