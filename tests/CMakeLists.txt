cmake_minimum_required(VERSION 3.13)
project(sdrpp_tests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(test_noise_floor_tracker test_noise_floor_tracker.cpp)
target_include_directories(test_noise_floor_tracker PRIVATE "../core/src")
add_test(NAME noise_floor_tracker COMMAND test_noise_floor_tracker)

add_executable(test_channel_scan test_channel_scan.cpp)
target_include_directories(test_channel_scan PRIVATE "../misc_modules/scanner/src")
add_test(NAME channel_scan COMMAND test_channel_scan)

add_executable(test_butterworth test_butterworth.cpp)
target_include_directories(test_butterworth PRIVATE "../core/src")
add_test(NAME butterworth COMMAND test_butterworth)

add_executable(test_spectrum_noise test_spectrum_noise.cpp)
target_include_directories(test_spectrum_noise PRIVATE "../core/src")
add_test(NAME spectrum_noise COMMAND test_spectrum_noise)

add_executable(test_synchronous_am test_synchronous_am.cpp)
target_include_directories(test_synchronous_am PRIVATE "../core/src")
add_test(NAME synchronous_am COMMAND test_synchronous_am)

find_package(PkgConfig)
pkg_check_modules(FFTW3 REQUIRED fftw3f)
add_executable(test_spectral_nr test_spectral_nr.cpp)
target_include_directories(test_spectral_nr PRIVATE "../core/src" ${FFTW3_INCLUDE_DIRS})
target_link_directories(test_spectral_nr PRIVATE ${FFTW3_LIBRARY_DIRS})
target_link_libraries(test_spectral_nr PRIVATE ${FFTW3_LIBRARIES})
add_test(NAME spectral_nr COMMAND test_spectral_nr)
