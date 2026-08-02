CMAKE_MINIMUM_REQUIRED(VERSION 3.14)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

set(QT_TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/uritests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/chartutilstests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/governance_dialog_tests.cpp
)

add_executable(test_organiclife-qt ${QT_TEST_SOURCES})
add_dependencies(test_organiclife-qt qt_stuff translations_target libunivalue libsecp256k1 libzcashrust leveldb crc32c bls)
target_include_directories(test_organiclife-qt PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/univalue/include
    ${CMAKE_SOURCE_DIR}/src/leveldb/include
    ${CMAKE_SOURCE_DIR}/src/leveldb/helpers/memenv
    ${CMAKE_SOURCE_DIR}/src/secp256k1/include
    ${CMAKE_SOURCE_DIR}/src/chiabls/src
    ${relic_SOURCE_DIR}/include
    ${relic_BINARY_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/rust/include
    ${BerkeleyDB_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
    ${sodium_INCLUDE_DIR}
    ${GMP_INCLUDE_DIR}
    ${LIBEVENT_INCLUDE_DIR}
)
target_link_libraries(test_organiclife-qt PRIVATE
    qt_stuff
    SERVER_A
    WALLET_A
    COMMON_A
    ZEROCOIN_A
    UTIL_A
    SAPLING_A
    BITCOIN_CRYPTO_A
    CLI_A
    univalue
    leveldb
    crc32c
    secp256k1
    rustzcash
    bls
    Qt${QT_VERSION_MAJOR}::Test
    ${BerkeleyDB_LIBRARIES} ${Boost_LIBRARIES} ${LIBEVENT_LIB}
    ${sodium_LIBRARY_RELEASE} ${GMP_LIBRARY}
    -ldl pthread
)
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    target_link_libraries(test_organiclife-qt PRIVATE "-framework Cocoa")
endif()
if(ZMQ_FOUND)
    target_link_libraries(test_organiclife-qt PRIVATE ZMQ_A ${ZMQ_LIB})
    target_include_directories(test_organiclife-qt PRIVATE ${ZMQ_INCLUDE_DIR})
endif()
if(MINIUPNP_FOUND)
    target_compile_definitions(test_organiclife-qt PRIVATE "-DSTATICLIB -DMINIUPNP_STATICLIB")
    target_link_libraries(test_organiclife-qt PRIVATE ${MINIUPNP_LIBRARY})
    target_include_directories(test_organiclife-qt PRIVATE ${MINIUPNP_INCLUDE_DIR})
endif()
if(NAT-PMP_FOUND)
    target_link_libraries(test_organiclife-qt PRIVATE ${NAT-PMP_LIBRARY})
    target_include_directories(test_organiclife-qt PRIVATE ${NAT-PMP_INCLUDE_DIR})
endif()

enable_testing()
add_test(NAME btest_organiclife-qt COMMAND test_organiclife-qt)
