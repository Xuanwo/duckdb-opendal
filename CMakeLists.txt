cmake_minimum_required(VERSION 3.11)

# Set extension name here
set(TARGET_NAME opendal)

include(FetchContent)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

set(EXTENSION_SOURCES
    src/opendal_extension.cpp
    src/opendal_file_system.cpp
    src/opendal_secret.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

set(OPENDAL_RS_FEATURES "services-s3,services-fs" CACHE STRING "Features for the Rust OpenDAL wrapper")
set(OPENDAL_RS_CARGO_TARGET_DIR "${CMAKE_BINARY_DIR}/opendal-rs-target")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(OPENDAL_RS_CARGO_BUILD_TYPE "debug")
  set(OPENDAL_RS_CARGO_BUILD_ARGS "")
else()
  set(OPENDAL_RS_CARGO_BUILD_TYPE "release")
  set(OPENDAL_RS_CARGO_BUILD_ARGS "--release")
endif()

if(APPLE)
  set(OPENDAL_RS_STATICLIB_NAME "libduckdb_opendal_ffi.a")
elseif(WIN32)
  set(OPENDAL_RS_STATICLIB_NAME "duckdb_opendal_ffi.lib")
else()
  set(OPENDAL_RS_STATICLIB_NAME "libduckdb_opendal_ffi.a")
endif()

set(OPENDAL_RS_STATICLIB_PATH "${OPENDAL_RS_CARGO_TARGET_DIR}/${OPENDAL_RS_CARGO_BUILD_TYPE}/${OPENDAL_RS_STATICLIB_NAME}")

add_custom_command(
  OUTPUT "${OPENDAL_RS_STATICLIB_PATH}"
  COMMAND ${CMAKE_COMMAND} -E env
          CARGO_TARGET_DIR=${OPENDAL_RS_CARGO_TARGET_DIR}
          cargo build --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/duckdb_opendal_ffi/Cargo.toml"
          ${OPENDAL_RS_CARGO_BUILD_ARGS} --features "${OPENDAL_RS_FEATURES}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM
)
add_custom_target(opendal_rs_build DEPENDS "${OPENDAL_RS_STATICLIB_PATH}")

add_library(duckdb_opendal_ffi STATIC IMPORTED GLOBAL)
set_target_properties(duckdb_opendal_ffi PROPERTIES IMPORTED_LOCATION "${OPENDAL_RS_STATICLIB_PATH}")
target_include_directories(duckdb_opendal_ffi INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/rust/duckdb_opendal_ffi/include")
add_dependencies(duckdb_opendal_ffi opendal_rs_build)
if(APPLE)
  target_link_libraries(duckdb_opendal_ffi INTERFACE "-framework CoreFoundation")
endif()

add_dependencies(${EXTENSION_NAME} opendal_rs_build)
add_dependencies(${LOADABLE_EXTENSION_NAME} opendal_rs_build)
target_link_libraries(${EXTENSION_NAME} duckdb_opendal_ffi)
target_link_libraries(${LOADABLE_EXTENSION_NAME} duckdb_opendal_ffi)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
