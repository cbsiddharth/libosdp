#
#  Copyright (c) 2020-2021 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

## Options
option(CONFIG_OSDP_PACKET_TRACE "Enable raw packet trace for diagnostics" OFF)
option(CONFIG_OSDP_SKIP_MARK_BYTE "Don't send the leading mark byte (0xFF)" OFF)
option(CONFIG_DISABLE_PRETTY_LOGGING "Don't colorize log ouputs" OFF)
option(CONFIG_OSDP_FILE "Enabled support for OSDP File Transfer" OFF)
option(CONFIG_OSDP_TRS "Enabled Transparent Reader Support" OFF)

if (CONFIG_OSDP_PACKET_TRACE)
	list(APPEND LIB_OSDP_DEFINITIONS "-DCONFIG_OSDP_PACKET_TRACE")
endif()

if (CONFIG_OSDP_SKIP_MARK_BYTE)
	list(APPEND LIB_OSDP_DEFINITIONS "-DCONFIG_OSDP_SKIP_MARK_BYTE")
endif()

if (CONFIG_DISABLE_PRETTY_LOGGING)
	list(APPEND LIB_OSDP_DEFINITIONS "-DCONFIG_DISABLE_PRETTY_LOGGING")
endif()

if (CONFIG_OSDP_STATIC_PD)
	list(APPEND LIB_OSDP_DEFINITIONS "-DCONFIG_OSDP_STATIC_PD")
endif()

if (CONFIG_OSDP_FILE)
	list(APPEND LIB_OSDP_DEFINITIONS "-DCONFIG_OSDP_FILE")
endif()

if (CONFIG_OSDP_TRS)
	list(APPEND LIB_OSDP_DEFINITIONS "-DCONFIG_OSDP_TRS")
endif()

# static and shared library names
set(LIB_OSDP_SHARED osdp)
set(LIB_OSDP_STATIC osdpstatic)

# optionally, find and use OpenSSL or MbedTLS
find_package(OpenSSL)

if (NOT OpenSSL_FOUND)
	find_package(MbedTLS)
endif()

# Generate osdp_config.h in build dir.
configure_file(osdp_config.h.in osdp_config.h @ONLY)

list(APPEND LIB_OSDP_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/osdp_common.c
	${CMAKE_CURRENT_SOURCE_DIR}/osdp_phy.c
	${CMAKE_CURRENT_SOURCE_DIR}/osdp_pd.c
	${CMAKE_CURRENT_SOURCE_DIR}/osdp_sc.c
)

if (NOT CONFIG_OSDP_STATIC_PD)
	list(APPEND LIB_OSDP_SOURCES
		${CMAKE_CURRENT_SOURCE_DIR}/osdp_cp.c
	)
endif()

if (CONFIG_OSDP_FILE)
	list(APPEND LIB_OSDP_SOURCES
		${CMAKE_CURRENT_SOURCE_DIR}/osdp_file.c
	)
endif()

if (CONFIG_OSDP_TRS)
	list(APPEND LIB_OSDP_SOURCES
		${CMAKE_CURRENT_SOURCE_DIR}/osdp_trs.c
	)
endif()

list(APPEND LIB_OSDP_INCLUDE_DIRS
	${PROJECT_SOURCE_DIR}/utils/include
	${PROJECT_SOURCE_DIR}/include
)

list(APPEND LIB_OSDP_PRIVATE_INCLUDE_DIRS
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_BINARY_DIR}
)

list(APPEND LIB_OSDP_LIBRARIES
	utils
)

if (OpenSSL_FOUND)
	list(APPEND LIB_OSDP_INCLUDE_DIRS ${OPENSSL_INCLUDE_DIR})
	list(APPEND LIB_OSDP_LIBRARIES ${OPENSSL_CRYPTO_LIBRARY})
	list(APPEND LIB_OSDP_DEFINITIONS "-DCONFIG_OSDP_USE_OPENSSL")
	list(APPEND LIB_OSDP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/crypto/openssl.c)
elseif (MbedTLS_FOUND)
	list(APPEND LIB_OSDP_INCLUDE_DIRS ${MBEDTLS_INCLUDE_DIRS})
	list(APPEND LIB_OSDP_LIBRARIES ${MBEDCRYPTO_LIBRARY})
	list(APPEND LIB_OSDP_DEFINITIONS "-DCONFIG_OSDP_USE_MBEDTLS")
	list(APPEND LIB_OSDP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/crypto/mbedtls.c)
else()
	list(APPEND LIB_OSDP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/crypto/tinyaes.c)
	list(APPEND LIB_OSDP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/crypto/tinyaes_src.c)
endif()

# These variables are used in other parts of this projects for rebuilding
# LibOSDP as they see fit. For instance tests are built with -DUNIT_TESTING
# and pyosdp is built by setuptools.
set(LIB_OSDP_SOURCES ${LIB_OSDP_SOURCES} PARENT_SCOPE)
set(LIB_OSDP_LIBRARIES ${LIB_OSDP_LIBRARIES} PARENT_SCOPE)
set(LIB_OSDP_INCLUDE_DIRS ${LIB_OSDP_INCLUDE_DIRS} PARENT_SCOPE)
set(LIB_OSDP_PRIVATE_INCLUDE_DIRS ${LIB_OSDP_PRIVATE_INCLUDE_DIRS} PARENT_SCOPE)
set(LIB_OSDP_DEFINITIONS ${LIB_OSDP_DEFINITIONS} PARENT_SCOPE)

add_definitions(${LIB_OSDP_DEFINITIONS})

## build libosdpstatic.a

add_library(${LIB_OSDP_STATIC} STATIC ${LIB_OSDP_SOURCES})
target_link_libraries(${LIB_OSDP_STATIC} ${LIB_OSDP_LIBRARIES})

target_include_directories(${LIB_OSDP_STATIC}
	PUBLIC
		$<INSTALL_INTERFACE:include>
		${LIB_OSDP_INCLUDE_DIRS}
	PRIVATE
		${LIB_OSDP_PRIVATE_INCLUDE_DIRS}
)

## build libosdp.so

# keep all symbols hidden by default (-fvisibility=hidden) for shared library
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET   hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

# For shared library (gcc/linux), utils must be recompiled with -fPIC. Right
# now cmake doesn't support `--whole-archvive ... --no-whole-archive` directive
# to linker (see https://gitlab.kitware.com/cmake/cmake/-/issues/20078).
#
# Note: Mac seems to be impervious to this issue, I suppose it treats all
# objects relocatable by default (?).
#
list(APPEND LIB_OSDP_UTILS_SRC
	${PROJECT_SOURCE_DIR}/utils/src/utils.c
	${PROJECT_SOURCE_DIR}/utils/src/list.c
	${PROJECT_SOURCE_DIR}/utils/src/queue.c
	${PROJECT_SOURCE_DIR}/utils/src/slab.c
)
set(LIB_OSDP_UTILS_SRC ${LIB_OSDP_UTILS_SRC} PARENT_SCOPE)

# add source files of utils instead of linking it. See comment above.
add_library(${LIB_OSDP_SHARED} SHARED ${LIB_OSDP_SOURCES} ${LIB_OSDP_UTILS_SRC})
if (OpenSSL_FOUND)
	target_link_libraries(${LIB_OSDP_SHARED} ${OPENSSL_CRYPTO_LIBRARY})
elseif (MbedTLS_FOUND)
	target_link_libraries(${LIB_OSDP_SHARED} ${MBEDCRYPTO_LIBRARY})
endif()

set_target_properties(${LIB_OSDP_SHARED} PROPERTIES
	VERSION ${PROJECT_VERSION}
	SOVERSION ${PROJECT_VERSION_MAJOR}
	PUBLIC_HEADER ${PROJECT_SOURCE_DIR}/include/osdp.h
)

target_include_directories(${LIB_OSDP_SHARED}
	PUBLIC
		$<INSTALL_INTERFACE:include>
		${LIB_OSDP_INCLUDE_DIRS}
	PRIVATE
		${LIB_OSDP_PRIVATE_INCLUDE_DIRS}
)

# generate osdp_export.h for OSDP_EXPORT() macro
# must be placed after the target definitions.
include(GenerateExportHeader)
generate_export_header(${LIB_OSDP_SHARED}
	EXPORT_MACRO_NAME OSDP_EXPORT
	EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/osdp_export.h
)

## Package Configuration

include(CMakePackageConfigHelpers)
set(LIBOSDP_CMAKE_CONFIG_DIR "lib/cmake/libosdp")
set(LIBOSDP_PKG_CONFIG_DIR   "lib/pkgconfig")
set(LIBOSDP_DEFINITIONS      ${LIB_OSDP_DEFINITIONS})
set(LIBOSDP_INCLUDE_DIR      "include")
set(LIBOSDP_LIBRARY          ${LIB_OSDP_SHARED})
set(LIBOSDP_LIBRARY_STATIC   ${LIB_OSDP_STATIC})
set(LIBOSDP_LIBRARY_DIR      "lib")
set(LIBOSDP_USE_FILE         "lib/cmake/libosdp/UseLibOSDP.cmake")
set(LIBOSDP_ROOT_DIR         ${CMAKE_INSTALL_PREFIX})
set(LIBOSDP_VERSION_STRING   ${PROJECT_VERSION})
set(LIBOSDP_VERSION_MAJOR    ${PROJECT_VERSION_MAJOR})
set(LIBOSDP_VERSION_MINOR    ${PROJECT_VERSION_MINOR})
set(LIBOSDP_VERSION_PATCH    ${PROJECT_VERSION_PATCH})

configure_package_config_file(
	${PROJECT_SOURCE_DIR}/cmake/LibOSDPConfig.cmake.in
	${CMAKE_BINARY_DIR}/LibOSDPConfig.cmake
	INSTALL_DESTINATION ${LIBOSDP_CMAKE_CONFIG_DIR}
	PATH_VARS
		LIBOSDP_ROOT_DIR
		LIBOSDP_INCLUDE_DIR
		LIBOSDP_LIBRARY_DIR
	NO_CHECK_REQUIRED_COMPONENTS_MACRO
	INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX}
)

write_basic_package_version_file (
	${CMAKE_BINARY_DIR}/LibOSDPConfigVersion.cmake
	VERSION ${PROJECT_VERSION}
	COMPATIBILITY AnyNewerVersion
)

# pkg-config file
configure_file(
	${PROJECT_SOURCE_DIR}/misc/libosdp.pc.in
	${CMAKE_BINARY_DIR}/libosdp.pc @ONLY
)

## Install targets

install(
	TARGETS ${LIB_OSDP_SHARED} ${LIB_OSDP_STATIC}
	LIBRARY DESTINATION ${LIBOSDP_LIBRARY_DIR}
	ARCHIVE DESTINATION ${LIBOSDP_LIBRARY_DIR}
	PUBLIC_HEADER DESTINATION ${LIBOSDP_INCLUDE_DIR}
	COMPONENT distributabes
)
install(
	FILES
		${CMAKE_BINARY_DIR}/LibOSDPConfig.cmake
		${CMAKE_BINARY_DIR}/LibOSDPConfigVersion.cmake
		${PROJECT_SOURCE_DIR}/cmake/UseLibOSDP.cmake
	DESTINATION ${LIBOSDP_CMAKE_CONFIG_DIR}
	COMPONENT config_files
)
install(
	FILES
		${CMAKE_BINARY_DIR}/libosdp.pc
	DESTINATION ${LIBOSDP_PKG_CONFIG_DIR}
	COMPONENT config_files
)
