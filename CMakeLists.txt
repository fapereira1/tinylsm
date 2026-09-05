cmake_minimum_required(VERSION 3.20)
project(tinylsm LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(tinylsm
    src/version.c src/arena.c src/skiplist.c
    src/crc32.c src/wal.c src/sst.c src/bloom.c
    src/db.c src/index.c)

target_include_directories(tinylsm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(tinylsm PRIVATE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -fno-omit-frame-pointer
)
target_link_libraries(tinylsm PUBLIC pthread m)   # <-- adicionado para o arena

enable_testing()

add_executable(tinylsm_cli src/main.c)
target_link_libraries(tinylsm_cli PRIVATE tinylsm)

add_executable(test_version tests/test_version.c)
target_link_libraries(test_version PRIVATE tinylsm)
add_test(NAME test_version COMMAND test_version)

# --- 1.1: slice + status (header-only, sem .c novo) ---
add_executable(test_slice tests/test_slice.c)
target_link_libraries(test_slice PRIVATE tinylsm)
add_test(NAME test_slice COMMAND test_slice)

# --- 1.2: Arena
add_executable(test_arena tests/test_arena.c)
target_link_libraries(test_arena PRIVATE tinylsm)
add_test(NAME test_arena COMMAND test_arena)

add_executable(test_skiplist tests/test_skiplist.c)
target_link_libraries(test_skiplist PRIVATE tinylsm)
add_test(NAME test_skiplist COMMAND test_skiplist)

add_executable(test_wal tests/test_wal.c)
target_link_libraries(test_wal PRIVATE tinylsm)
add_test(NAME test_wal COMMAND test_wal)

add_executable(test_sst tests/test_sst.c)
target_link_libraries(test_sst PRIVATE tinylsm)
add_test(NAME test_sst COMMAND test_sst)

add_executable(test_bloom tests/test_bloom.c)
target_link_libraries(test_bloom PRIVATE tinylsm)
add_test(NAME test_bloom COMMAND test_bloom)

add_executable(test_db tests/test_db.c)
target_link_libraries(test_db PRIVATE tinylsm)
add_test(NAME test_db COMMAND test_db)

add_executable(test_compaction tests/test_compaction.c)
target_link_libraries(test_compaction PRIVATE tinylsm)
add_test(NAME test_compaction COMMAND test_compaction)

add_executable(test_index tests/test_index.c)
target_link_libraries(test_index PRIVATE tinylsm)
add_test(NAME test_index COMMAND test_index)

add_executable(test_mvcc tests/test_mvcc.c)
target_link_libraries(test_mvcc PRIVATE tinylsm)
add_test(NAME test_mvcc COMMAND test_mvcc)

# --- Crash recovery (fork + _exit simula crash sem db_close) ---
add_executable(test_crash_recovery tests/test_crash_recovery.c)
target_link_libraries(test_crash_recovery PRIVATE tinylsm)
add_test(NAME test_crash_recovery COMMAND test_crash_recovery)

# _exit() no filho bypassa os atexit handlers do ASan — sem leaks reais,
# mas o ASan reportaria como falso positivo. Suprime só detect_leaks.
set_tests_properties(test_crash_recovery PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

# --- Fuzzer (requer -DFUZZER=ON via preset fuzzer) ---
option(FUZZER "Build fuzz targets" OFF)
if(FUZZER)
    # Versão instrumented da lib para o fuzzer
    add_library(tinylsm_fuzz STATIC
        src/version.c src/arena.c src/skiplist.c
        src/crc32.c src/wal.c src/sst.c src/bloom.c
        src/db.c src/index.c)
    target_include_directories(tinylsm_fuzz PUBLIC include src)
    target_compile_options(tinylsm_fuzz PRIVATE
        ${WARN_FLAGS} -fsanitize=fuzzer-no-link,address -g)
    target_link_libraries(tinylsm_fuzz PUBLIC pthread m)

    add_executable(fuzz_db fuzz/fuzz_db.c)
    target_link_libraries(fuzz_db PRIVATE tinylsm_fuzz)
    target_compile_options(fuzz_db PRIVATE -fsanitize=fuzzer,address -g)
    target_link_options(fuzz_db PRIVATE -fsanitize=fuzzer,address)
endif()
