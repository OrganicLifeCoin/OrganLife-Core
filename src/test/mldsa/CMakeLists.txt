cmake_minimum_required(VERSION 3.14)
project(OrganicLifeMLDSATests LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Boost CONFIG REQUIRED COMPONENTS filesystem)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
find_package(Threads REQUIRED)

get_filename_component(OLC_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(PQ_SOURCES
    ${OLC_SRC}/wallet/pqkey.cpp
    ${OLC_SRC}/wallet/pqcredentials_win.cpp
    ${OLC_SRC}/pqaddress.cpp
    ${OLC_SRC}/bech32.cpp
    ${OLC_SRC}/uint256.cpp
    ${OLC_SRC}/utilstrencodings.cpp
    ${OLC_SRC}/crypto/sha256.cpp
    ${OLC_SRC}/crypto/mldsa44.cpp
    ${OLC_SRC}/crypto/mldsa44_native.c
    ${OLC_SRC}/support/lockedpool.cpp
    ${OLC_SRC}/support/cleanse.cpp)
add_executable(test_mldsa ../mldsa_tests.cpp ../pqkey_tests.cpp ${PQ_SOURCES})
target_compile_definitions(test_mldsa PRIVATE MLDSA_STANDALONE_TEST)
target_include_directories(test_mldsa PRIVATE ${OLC_SRC} ${Boost_INCLUDE_DIRS})
target_link_libraries(test_mldsa PRIVATE PkgConfig::SODIUM Threads::Threads Boost::filesystem)
if(WIN32)
    target_compile_definitions(test_mldsa PRIVATE WIN32)
    target_link_libraries(test_mldsa PRIVATE advapi32)
    add_executable(test_pqcredentials ../pqcredentials_win_tests.cpp ${PQ_SOURCES})
    target_compile_definitions(test_pqcredentials PRIVATE WIN32 PQ_CREDENTIAL_CRYPTO_TEST)
    if(MSVC)
        target_compile_options(test_pqcredentials PRIVATE /UNDEBUG)
    else()
        target_compile_options(test_pqcredentials PRIVATE -UNDEBUG)
    endif()
    target_include_directories(test_pqcredentials PRIVATE ${OLC_SRC} ${Boost_INCLUDE_DIRS})
    target_link_libraries(test_pqcredentials PRIVATE PkgConfig::SODIUM Threads::Threads Boost::filesystem advapi32)
    if(MINGW)
        target_compile_definitions(test_pqcredentials PRIVATE PQ_CREDENTIAL_FAULT_TEST)
        target_link_options(test_pqcredentials PRIVATE
            -Wl,--wrap=__imp_WriteFile -Wl,--wrap=__imp_FlushFileBuffers -Wl,--wrap=__imp_MoveFileExW)
    endif()
endif()

enable_testing()
add_test(NAME mldsa44 COMMAND test_mldsa)
if(WIN32)
    add_test(NAME pqcredentials COMMAND test_pqcredentials)
endif()
