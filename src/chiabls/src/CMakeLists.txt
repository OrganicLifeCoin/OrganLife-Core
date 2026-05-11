CMAKE_MINIMUM_REQUIRED(VERSION 3.14 FATAL_ERROR)
set (CMAKE_CXX_STANDARD 14)

# CMake 3.14+
include(FetchContent)

if (DEFINED ENV{RELIC_MAIN})
  set(RELIC_GIT_TAG "origin/main")
else ()
  set(RELIC_GIT_TAG "e6209fd80e07203b865983faee635fa1f85d6c9f")
endif ()

message(STATUS "Relic will be built from: ${RELIC_GIT_TAG}")

set(RELIC_GIT_REPOSITORY "https://github.com/relic-toolkit/relic.git" CACHE STRING "Relic git repository URL")
if(DEFINED ENV{RELIC_GIT_REPOSITORY} AND NOT "$ENV{RELIC_GIT_REPOSITORY}" STREQUAL "")
  set(RELIC_GIT_REPOSITORY "$ENV{RELIC_GIT_REPOSITORY}")
endif()

set(RELIC_SOURCE_DIR "$ENV{RELIC_SOURCE_DIR}")
if(NOT "${RELIC_SOURCE_DIR}" STREQUAL "" AND EXISTS "${RELIC_SOURCE_DIR}/CMakeLists.txt")
  message(STATUS "Using local relic source dir: ${RELIC_SOURCE_DIR}")
  FetchContent_Declare(
    relic
    SOURCE_DIR "${RELIC_SOURCE_DIR}"
  )
else()
  FetchContent_Declare(
    relic
    GIT_REPOSITORY ${RELIC_GIT_REPOSITORY}
    GIT_TAG        ${RELIC_GIT_TAG}
  )
endif()
FetchContent_MakeAvailable(relic)

file(GLOB HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)
source_group("SrcHeaders" FILES ${HEADERS})

include_directories(
  ${INCLUDE_DIRECTORIES}
  ${relic_SOURCE_DIR}/include
  ${relic_BINARY_DIR}/include
  )

if (GMP_FOUND)
  include_directories(${GMP_INCLUDES})
endif(GMP_FOUND)

set(C_LIB ${CMAKE_BINARY_DIR}/libbls.a)

add_library(bls ${CMAKE_CURRENT_SOURCE_DIR}/privatekey.cpp)

add_library(blstmp ${HEADERS}
  ${CMAKE_CURRENT_SOURCE_DIR}/privatekey.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/bls.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/elements.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/schemes.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/threshold.cpp
)

set(OPREFIX object_)
find_library(GMP_NAME NAMES libgmp.a gmp)
find_library(SODIUM_NAME NAMES libsodium.a sodium)

set(LIBRARIES_TO_COMBINE
      COMMAND cd ${CMAKE_BINARY_DIR} && mkdir -p ${OPREFIX}$<TARGET_NAME:blstmp> && cd ${OPREFIX}$<TARGET_NAME:blstmp> && ${CMAKE_AR} -x $<TARGET_FILE:blstmp>
      COMMAND cd ${CMAKE_BINARY_DIR} && mkdir -p ${OPREFIX}$<TARGET_NAME:relic_s> && cd ${OPREFIX}$<TARGET_NAME:relic_s> && ${CMAKE_AR} -x $<TARGET_FILE:relic_s>
)

if (GMP_FOUND)
  list(APPEND LIBRARIES_TO_COMBINE COMMAND cd ${CMAKE_BINARY_DIR} && mkdir -p ${OPREFIX}gmp && cd ${OPREFIX}gmp && ${CMAKE_AR} -x ${GMP_NAME})
endif()
if (SODIUM_FOUND)
  message("SODIUM_FOUND in src/CMakeLists.txt")
  list(APPEND LIBRARIES_TO_COMBINE COMMAND cd ${CMAKE_BINARY_DIR} && mkdir -p ${OPREFIX}sodium && cd ${OPREFIX}sodium && ${CMAKE_AR} -x ${SODIUM_NAME})
  target_compile_definitions(blstmp PRIVATE BLSALLOC_SODIUM=1)
else()
  target_compile_definitions(blstmp PRIVATE)
endif()

add_custom_target(combined_custom
        ${LIBRARIES_TO_COMBINE}
        COMMAND cd ${CMAKE_BINARY_DIR} && ${CMAKE_AR} -rs ${C_LIB} ${OPREFIX}*/*${CMAKE_C_OUTPUT_EXTENSION}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS blstmp relic_s
        )

add_library(combined STATIC IMPORTED GLOBAL)
add_dependencies(combined combined_custom)
target_link_libraries(bls combined)

set_target_properties(combined
        PROPERTIES
        IMPORTED_LOCATION ${C_LIB}
        )

file(GLOB includes "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
install(DIRECTORY ${relic_SOURCE_DIR}/include/ DESTINATION include/chiabls)
install(DIRECTORY ${relic_BINARY_DIR}/include/ DESTINATION include/chiabls)
install(FILES ${includes} DESTINATION include/chiabls)
install(FILES ${C_LIB} DESTINATION lib)

if (BUILD_BLS_TESTS)
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../contrib/catch)
  add_executable(runtest test.cpp)
  if (SODIUM_FOUND)
    target_link_libraries(runtest blstmp relic_s sodium)
  else()
    target_link_libraries(runtest blstmp relic_s)
  endif()
endif()

if (BUILD_BLS_BENCHMARKS)
  add_executable(runbench test-bench.cpp)
  if (SODIUM_FOUND)
    target_link_libraries(runbench blstmp relic_s sodium)
  else()
    target_link_libraries(runbench blstmp relic_s)
  endif()
endif()
