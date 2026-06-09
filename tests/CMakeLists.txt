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

add_executable(sync_tests
    sync/test_smoke.cpp
    sync/test_logical_attribute.cpp
    sync/test_operational_mode.cpp
    sync/test_aai_translation.cpp
    sync/test_twin_field.cpp
    sync/test_logical_ac_state.cpp
    sync/test_reconciler.cpp
    sync/test_projector.cpp
    sync/test_atomic_buffer.cpp
    sync/test_integration.cpp
    sync/test_threading.cpp
    sync/test_reachable.cpp
    sync/test_change_publisher.cpp
    sync/test_bridge_kernel.cpp
    ${SRC_DIR}/sync/projector.cpp
    ${SRC_DIR}/sync/reconciler.cpp
    ${SRC_DIR}/sync/atomic_buffer.cpp
    ${SRC_DIR}/sync/change_publisher.cpp
)

target_include_directories(sync_tests PRIVATE
    ${SRC_DIR}/sync                                  # sync/ production headers
    ${SRC_DIR}/s21                                   # OperatingMode / FanMode enums
    ${CMAKE_SOURCE_DIR}/sync/chip_stubs              # stub chip:: types
    ${CMAKE_SOURCE_DIR}/../third_party               # tl/expected.hpp
)

target_link_libraries(sync_tests PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(sync_tests)

# ─── sync_kernel_chip_free: build-only check that pins the §3 decoupling ─────
# Compiles the kernel translation units (Projector, Reconciler, AtomicTxn)
# with the s21/ and sync/ headers visible but **no** chip/… on the include
# path. If any kernel TU regresses to reaching for a CHIP header, this
# target fails its build, which CTest treats as a failure.
add_library(sync_kernel_chip_free OBJECT
    ${SRC_DIR}/sync/projector.cpp
    ${SRC_DIR}/sync/reconciler.cpp
    ${SRC_DIR}/sync/atomic_buffer.cpp
    ${SRC_DIR}/sync/change_publisher.cpp
)
target_include_directories(sync_kernel_chip_free PRIVATE
    ${SRC_DIR}/sync
    ${SRC_DIR}/s21
    ${CMAKE_SOURCE_DIR}/../third_party
)
set_target_properties(sync_kernel_chip_free PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_test(NAME sync_kernel_chip_free_build
         COMMAND ${CMAKE_COMMAND}
                 --build ${CMAKE_BINARY_DIR}
                 --target sync_kernel_chip_free)
