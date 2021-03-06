#
# This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

project(Firestorm_TBC)

# Define here name of the binaries
set(BINARY_SERVER_NAME "worldserver")
set(BINARY_REALM_NAME "realmserver")

cmake_minimum_required(VERSION 2.8.12)

include(cmake/common.cmake)

# Set RPATH-handing (CMake parameters)
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH ${LIBS_DIR})
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# set default buildoptions and print them
include(cmake/options.cmake)

# Force out-of-source build
if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
  message(FATAL_ERROR
    "This project requires an out of source build. Remove the file 'CMakeCache.txt' found in this directory before continuing, create a separate build directory and run 'cmake [options] <srcs>' from there."
  )
endif()

include(CheckPlatform)

# Find out what system we use to include the needed libs set(CMAKE_BUILD_TYPE "Release")
if(WIN32)		
  if(PLATFORM MATCHES X86) # 32-bit		
    set(DEP_ARCH Win32)		
  else() # 64-bit		
    set(DEP_ARCH x64)		
  endif()		
endif()

# build in Release-mode by default if not explicitly set
if( NOT CMAKE_BUILD_TYPE )
  set(CMAKE_BUILD_TYPE "Release")
endif()

# find Git: used to get the revision number
find_package(Git)

if(NOT MSVC)
  # Include cotire to manage PCH support
  include(cotire)
endif()

set(BIN_FOLDER_NAME bin)
set(CONF_FOLDER_NAME etc)
set(LIBS_FOLDER_NAME lib)

# Set Install folders
set(BIN_DIR ${CMAKE_INSTALL_PREFIX}/${BIN_FOLDER_NAME})
set(CONF_DIR ${CMAKE_INSTALL_PREFIX}/${CONF_FOLDER_NAME})
set(LIBS_DIR ${CMAKE_INSTALL_PREFIX}/${LIBS_FOLDER_NAME})

# On windows define more developpement folders and install provided libs
if(WIN32)
  set(BIN_DIR ${CMAKE_INSTALL_PREFIX})
  set(LIBS_DIR ${BIN_DIR})
  set(CONF_DIR ${BIN_DIR})
  if(MSVC)
    set(DEV_BIN_DIR ${CMAKE_SOURCE_DIR}/${BIN_FOLDER_NAME}/${DEP_ARCH}_$(Configuration))
    set(DEV_PROVIDED_LIBS_FOLDER ${CMAKE_SOURCE_DIR}/dep/lib/${DEP_ARCH}_$(Configuration))
  else()
    set(DEV_BIN_DIR ${CMAKE_SOURCE_DIR}/${BIN_FOLDER_NAME}/${DEP_ARCH}_${CMAKE_BUILD_TYPE})
    set(DEV_PROVIDED_LIBS_FOLDER ${CMAKE_SOURCE_DIR}/dep/lib/${DEP_ARCH}_${CMAKE_BUILD_TYPE})
  endif()

  # Define install rules for provided libs
  install(
    FILES
      ${DEV_PROVIDED_LIBS_FOLDER}/libeay32.dll
      ${DEV_PROVIDED_LIBS_FOLDER}/libmySQL.dll
    DESTINATION ${LIBS_DIR}
  )

  # TODO:: check why x64 is not provided
  if(PLATFORM MATCHES X86)
    install(
      FILES
        ${DEV_PROVIDED_LIBS_FOLDER}/dbghelp.dll
      DESTINATION ${LIBS_DIR}
    )
  endif()
endif()

# For Unix systems set the rpath so that libraries are found
set(CMAKE_INSTALL_RPATH ../${LIBS_FOLDER_NAME})
set(CMAKE_INSTALL_NAME_DIR ${LIBS_DIR})

# Add alternate lib dir for boost on windows
if(WIN32 AND NOT "$ENV{BOOST_ROOT}" STREQUAL "")
  set(BOOST_LIBRARYDIR "$ENV{BOOST_LIBRARYDIR}")
  set(BOOST_LIBRARYDIR "${BOOST_LIBRARYDIR};$ENV{BOOST_ROOT}\\lib\\${DEP_ARCH}")
  set(BOOST_LIBRARYDIR "${BOOST_LIBRARYDIR};$ENV{BOOST_ROOT}\\stage\\${DEP_ARCH}")
  set(BOOST_LIBRARYDIR "${BOOST_LIBRARYDIR};$ENV{BOOST_ROOT}\\stage")
endif()

# Be sure to search only static lib
add_definitions(-DBOOST_ALL_NO_LIB)
set(Boost_USE_STATIC_LIBS ON)
#set(Boost_DEBUG ON)

if(MSVC)
  # Use at least verion 3.8.1 of FindBoost to fix some issues. Especialy in MSVC with fixed Toolset issues.
  # This is a workaround while VisualStudio does not embed higher cmake version than 3.7.2
  if(${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION}.${CMAKE_PATCH_VERSION} LESS 3.8.1)
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake/macros/FindBoost)
    message(STATUS "FindBoost: included version 3.8.1 of FindBoost is used.")
  endif()
endif()

# Find needed packages and if necessery abort if something important is missing
find_package(Boost REQUIRED COMPONENTS system program_options thread regex)

if(Boost_FOUND)
    include_directories(${Boost_INCLUDE_DIRS})
    link_libraries(${Boost_LIBRARIES})
else()
  message(STATUS "BOOST_ROOT = $ENV{BOOST_ROOT}")
  message(STATUS "BOOST_LIBRARYDIR = $ENV{BOOST_LIBRARYDIR}")
  if ("$ENV{BOOST_ROOT}" STREQUAL "")
    message(WARNING "BOOST_ROOT is not set, please set it correctly to your boost folder.")
  else()
    if ("$ENV{BOOST_LIBRARYDIR}" STREQUAL "")
        message(STATUS "Please put all ${DEP_ARCH} boost lib files in $ENV{BOOST_ROOT}\\lib\\${DEP_ARCH} folder")
        message(STATUS "Or set BOOST_LIBRARYDIR to your boost lib folder.")
    else()
        message(STATUS "BOOST_LIBRARYDIR is set, please verify if correct libs files are in that folder.")
    endif()
  endif()
  message(FATAL_ERROR "This project requires boost.  Please install from http://www.boost.org")
endif()

# Win32 delivered packages
if(WIN32)
  set(MYSQL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/dep/include")
  set(MYSQL_LIBRARY "${CMAKE_SOURCE_DIR}/dep/lib/${DEP_ARCH}_release/libmySQL.lib")
  set(MYSQL_DEBUG_LIBRARY "${CMAKE_SOURCE_DIR}/dep/lib/${DEP_ARCH}_debug/libmySQL.lib")
  set(OPENSSL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/dep/include")
  set(OPENSSL_LIBRARIES "${CMAKE_SOURCE_DIR}/dep/lib/${DEP_ARCH}_release/libeay32.lib")
  set(OPENSSL_DEBUG_LIBRARIES "${CMAKE_SOURCE_DIR}/dep/lib/${DEP_ARCH}_debug/libeay32.lib")
  set(ZLIB_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/dep/zlib")
  set(ZLIB_LIBRARIES "z")
  set(BZIP2_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/dep/src/bzip2")
  set(BZIP2_LIBRARIES "bzip2")
endif()

# *nix-specific packages
if(UNIX)
  if(POSTGRESQL)
    find_package(PostgreSQL REQUIRED)

    if(POSTGRESQL_FOUND)
      include_directories(${POSTGRESQL_INCLUDE_DIRS})
    endif(POSTGRESQL_FOUND)

  else()
    find_package(MySQL REQUIRED)
  endif()

  # If OpenSSL path isn't specified on mac we set the one that homebrew uses
  # since that's what most people will be using.
  find_package(OpenSSL 1.0 REQUIRED)
  if(NOT OPENSSL_VERSION VERSION_LESS "1.1")
    message(FATAL_ERROR "You have unsupported version of OpenSSL library. Use version below 1.1")
  endif()
  find_package(ZLIB REQUIRED)
  if(BUILD_EXTRACTORS)
    find_package(BZip2 REQUIRED)
  endif()
endif()

# Find core revision
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_REVISION
    RESULT_VARIABLE GIT_RESULT
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(GIT_RESULT)
    set(GIT_REVISION "Git repository not found")
  endif()
else()
  set(GIT_REVISION "Git not found")
endif()

set(DEFINITIONS_RELEASE NDEBUG)
set(DEFINITIONS_DEBUG _DEBUG MANGOS_DEBUG)

if(NOT BUILD_GAME_SERVER AND BUILD_SCRIPTDEV)
  set(BUILD_SCRIPTDEV OFF)
  message(STATUS "BUILD_SCRIPTDEV forced to OFF due to BUILD_GAME_SERVER is not set")
endif()

# print out the results before continuing
include(cmake/showoptions.cmake)

if(NOT BUILD_GAME_SERVER AND NOT BUILD_LOGIN_SERVER AND NOT BUILD_EXTRACTORS)
  message(FATAL_ERROR "You must select something to build!")
endif()

add_subdirectory(dep)

# Add definitions for all build types
# Don't place this above 'dep' subdirectory! Because of defines build will crash.
set(DEFINITIONS "")

if(UNIX)
  set(DEFINITIONS SYSCONFDIR="../${CONF_FOLDER_NAME}/")
endif()

if(POSTGRESQL)
  set(DEFINITIONS ${DEFINITIONS} DO_POSTGRESQL)
else()
  set(DEFINITIONS ${DEFINITIONS} DO_MYSQL)
endif()

if(BUILD_GAME_SERVER OR BUILD_LOGIN_SERVER)
  add_subdirectory(src)
  if(MSVC AND BUILD_GAME_SERVER)
    set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${BINARY_SERVER_NAME})
  endif()

  # Generate revision-extractor
  set(GENREV_SRC
    src/tools/genrevision/genrevision.cpp
  )

  add_executable(RevisionGenerator
    ${GENREV_SRC}
  )
endif()

if(BUILD_EXTRACTORS)
  add_subdirectory(contrib/extractor)
  add_subdirectory(contrib/vmap_extractor)
  add_subdirectory(contrib/vmap_assembler)
  add_subdirectory(contrib/mmap)
endif()

# if(SQL)
#   add_subdirectory(sql)
# endif()
