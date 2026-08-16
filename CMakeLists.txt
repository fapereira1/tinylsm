cmake_minimum_required(VERSION 3.20)
project(tinylsm LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(tinylsm src/version.c src/arena.c src/skiplist.c)
target_include_directories(tinylsm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(tinylsm PRIVATE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -fno-omit-frame-pointer
)
target_link_libraries(tinylsm PUBLIC pthread)   # <-- adicionado para o arena


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
