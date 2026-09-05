cmake_minimum_required(VERSION 3.14)
project(OrganicLifeMLDSATests LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Boost CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
find_package(Threads REQUIRED)

get_filename_component(OLC_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_executable(test_mldsa
    ../mldsa_tests.cpp
    ../pqkey_tests.cpp
    ${OLC_SRC}/wallet/pqkey.cpp
    ${OLC_SRC}/pqaddress.cpp
    ${OLC_SRC}/bech32.cpp
    ${OLC_SRC}/crypto/sha256.cpp
    ${OLC_SRC}/crypto/mldsa44.cpp
    ${OLC_SRC}/crypto/mldsa44_native.c
    ${OLC_SRC}/support/lockedpool.cpp
    ${OLC_SRC}/support/cleanse.cpp)
target_compile_definitions(test_mldsa PRIVATE MLDSA_STANDALONE_TEST)
target_include_directories(test_mldsa PRIVATE ${OLC_SRC} ${Boost_INCLUDE_DIRS})
target_link_libraries(test_mldsa PRIVATE PkgConfig::SODIUM Threads::Threads)

enable_testing()
add_test(NAME mldsa44 COMMAND test_mldsa)
