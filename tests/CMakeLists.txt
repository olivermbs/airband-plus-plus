cmake_minimum_required(VERSION 3.13)
project(sdrpp_tests)

add_executable(test_noise_floor_tracker test_noise_floor_tracker.cpp)
target_include_directories(test_noise_floor_tracker PRIVATE "../core/src")
add_test(NAME noise_floor_tracker COMMAND test_noise_floor_tracker)

add_executable(test_channel_scan test_channel_scan.cpp)
target_include_directories(test_channel_scan PRIVATE "../misc_modules/scanner/src")
add_test(NAME channel_scan COMMAND test_channel_scan)

add_executable(test_butterworth test_butterworth.cpp)
target_include_directories(test_butterworth PRIVATE "../core/src")
add_test(NAME butterworth COMMAND test_butterworth)
