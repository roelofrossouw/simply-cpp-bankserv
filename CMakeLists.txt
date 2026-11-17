cmake_minimum_required(VERSION 3.22)
set(SC_MODULE bankserv)
project(simply-cpp-${SC_MODULE} HOMEPAGE_URL "https://github.com/roelofrossouw/simply-cpp-bankserv" LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_BANKSERV_BUILD_TESTS "Build simply-cpp-bankserv tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

# sc-core for the helpers and the example's sc::params; the reader itself uses only the standard library.
find_or_install_package(sc-core simply-cpp-dev simply-cpp 3.0.0)

set(SOURCE_OBJECTS)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

add_sc_object(load_report)

add_sc_libraries(DESCRIPTION "simply-cpp Bankserv (ACB) files: load reports read into fields, records and totals")
target_compile_features(sc-bankserv PUBLIC cxx_std_20)
target_compile_features(sc-bankserv-shared PUBLIC cxx_std_20)

install_sc_module(PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
install(FILES cmake/SimplyCppFunctions.cmake cmake/sc_bootstrap.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sc-bankserv
        COMPONENT development)

if (SC_BANKSERV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests sc-bankserv-test)
endif ()

add_sc_example(sc-bankserv-demo LINK_LIBRARIES sc::sc-core sc::sc-bankserv-shared)

package_sc_module(DEV_DEPENDS "simply-cpp-dev (>= 3.0.0~)")
