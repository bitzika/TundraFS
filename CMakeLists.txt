cmake_minimum_required(VERSION 3.10)
project(TundraFS C)

set(CMAKE_C_STANDARD 11)
include_directories(include)

add_executable(tundra_fs
    src/tundra_fs.c
    src/tundra_cow.c
    src/tundra_trim.c
)
target_link_libraries(tundra_fs pthread m)

add_executable(stress_test tools/stress_test.c)
target_link_libraries(stress_test pthread)

add_executable(mkfs_tundra tools/mkfs_tundra.c)
target_link_libraries(mkfs_tundra pthread)

add_executable(tundra_fsck tools/tundra_fsck.c)
target_link_libraries(tundra_fsck pthread)

add_executable(tundra_fuse tools/tundra_fuse.c)
target_link_libraries(tundra_fuse fuse3 pthread)
