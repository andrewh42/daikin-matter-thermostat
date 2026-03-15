cmake_minimum_required(VERSION 3.20.0)
project(s21_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
)
FetchContent_MakeAvailable(Catch2)
include(CTest)
include(Catch)

set(SRC_DIR ${CMAKE_SOURCE_DIR}/../src)

add_executable(s21_tests
    s21/test_s21_frame.cpp
    s21/test_s21_presentation.cpp
    ${SRC_DIR}/s21/s21_frame.cpp
    ${SRC_DIR}/s21/s21_presentation.cpp
)

target_include_directories(s21_tests PRIVATE
    ${SRC_DIR}/s21              # S21DataLink.h, S21Frame.h, S21Presentation.h
    ${CMAKE_SOURCE_DIR}/s21     # mock_s21_datalink.h
    ${CMAKE_SOURCE_DIR}/../third_party  # tl/expected.hpp
)

target_link_libraries(s21_tests PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(s21_tests)

add_executable(s21_manager_tests
    s21/test_s21_manager.cpp
    ${SRC_DIR}/s21/s21_manager.cpp
)

target_include_directories(s21_manager_tests PRIVATE
    ${SRC_DIR}/s21
    ${CMAKE_SOURCE_DIR}/s21
    ${CMAKE_SOURCE_DIR}/../third_party
)

target_link_libraries(s21_manager_tests PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(s21_manager_tests)

add_executable(s21_to_matter_translator_tests
    s21_to_matter_translator/test_s21_to_matter_translator.cpp
    ${SRC_DIR}/s21_to_matter_translator.cpp
)

target_include_directories(s21_to_matter_translator_tests PRIVATE
    ${SRC_DIR}                                                        # s21_to_matter_translator.h
    ${SRC_DIR}/s21                                                    # s21_presentation.h (for OperatingMode, FanMode)
    ${CMAKE_SOURCE_DIR}/s21_to_matter_translator/chip_stubs           # stub chip::app::Clusters::Thermostat enums
    ${CMAKE_SOURCE_DIR}/../third_party                                # tl/expected.hpp
)

target_link_libraries(s21_to_matter_translator_tests PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(s21_to_matter_translator_tests)
