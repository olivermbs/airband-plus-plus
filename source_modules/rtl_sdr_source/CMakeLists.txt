cmake_minimum_required(VERSION 3.13)
project(rtl_sdr_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

if (MSVC)
    # Lib path
    target_link_directories(rtl_sdr_source PRIVATE "C:/Program Files/PothosSDR/bin/")

    target_link_libraries(rtl_sdr_source PRIVATE rtlsdr)
elseif (ANDROID)
    target_link_libraries(rtl_sdr_source PUBLIC
        /sdr-kit/${ANDROID_ABI}/lib/libusb1.0.so
        /sdr-kit/${ANDROID_ABI}/lib/librtlsdr.so
    )
elseif (OPT_BUILD_BUNDLED_LIBRTLSDR)
    find_package(PkgConfig)

    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

    # Static and with hidden symbols, so it can't clash with a system librtlsdr loaded by another module
    file(GLOB LIBRTLSDR_SRC "librtlsdr/src/*.c")
    add_library(rtlsdr_bundled STATIC ${LIBRTLSDR_SRC})
    set_target_properties(rtlsdr_bundled PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
    target_compile_definitions(rtlsdr_bundled PUBLIC rtlsdr_STATIC)
    target_include_directories(rtlsdr_bundled PUBLIC "librtlsdr/include" ${LIBUSB_INCLUDE_DIRS})
    target_link_directories(rtlsdr_bundled PUBLIC ${LIBUSB_LIBRARY_DIRS})
    target_link_libraries(rtlsdr_bundled PUBLIC ${LIBUSB_LIBRARIES})

    target_link_libraries(rtl_sdr_source PRIVATE rtlsdr_bundled)
else (MSVC)
    find_package(PkgConfig)

    pkg_check_modules(LIBRTLSDR REQUIRED librtlsdr)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

    target_include_directories(rtl_sdr_source PRIVATE ${LIBRTLSDR_INCLUDE_DIRS} ${LIBUSB_INCLUDE_DIRS})
    target_link_directories(rtl_sdr_source PRIVATE ${LIBRTLSDR_LIBRARY_DIRS} ${LIBUSB_LIBRARY_DIRS})
    target_link_libraries(rtl_sdr_source PRIVATE ${LIBRTLSDR_LIBRARIES} ${LIBUSB_LIBRARIES})
endif ()