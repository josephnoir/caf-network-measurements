cmake_minimum_required(VERSION 2.8)
project(indexing C CXX)

set (CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# silence policy CMP0042 warning by enabling RPATH explicitly
if(APPLE AND NOT DEFINED CMAKE_MACOSX_RPATH)
  set(CMAKE_MACOSX_RPATH true)
endif()

# prohibit in-source builds
if ("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
    message(FATAL_ERROR "In-source builds are not allowed. Please use "
                        "./configure to choose a build directory and "
                        "initialize the build configuration.")
endif ()

if(CAF_ENABLE_ADDRESS_SANITIZER)
  # check whether address sanitizer is available
  set(CXXFLAGS_BACKUP "${CMAKE_CXX_FLAGS}")
  set(CMAKE_CXX_FLAGS "-fsanitize=address -fno-omit-frame-pointer")
  try_run(ProgramResult
          CompilationSucceeded
          "${CMAKE_CURRENT_BINARY_DIR}"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/get_compiler_version.cpp")
  if(NOT CompilationSucceeded)
    message(WARNING "Address Sanitizer is not available on selected compiler")
  else()
    message(STATUS "Enable Address Sanitizer")
    set(EXTRA_FLAGS "${EXTRA_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
  endif()
  # restore CXX flags
  set(CMAKE_CXX_FLAGS "${CXXFLAGS_BACKUP}")
endif(CAF_ENABLE_ADDRESS_SANITIZER)

# check if the user provided CXXFLAGS, set defaults otherwise
if(NOT CMAKE_CXX_FLAGS)
  set(CMAKE_CXX_FLAGS                   "-std=c++14 -Wextra -Wall -pedantic ${EXTRA_FLAGS}")
endif()
if(NOT CMAKE_CXX_FLAGS_DEBUG)
  set(CMAKE_CXX_FLAGS_DEBUG             "-O0 -g")
endif()
if(NOT CMAKE_CXX_FLAGS_MINSIZEREL)
  set(CMAKE_CXX_FLAGS_MINSIZEREL        "-Os")
endif()
if(NOT CMAKE_CXX_FLAGS_RELEASE)
  set(CMAKE_CXX_FLAGS_RELEASE           "-O3 -DNDEBUG")
endif()
if(NOT CMAKE_CXX_FLAGS_RELWITHDEBINFO)
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO    "-O2 -g")
endif()

# set build default build type to RelWithDebInfo if not set
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

string(TOUPPER "${CMAKE_BUILD_TYPE}" UPPER_BUILD_TYPE)
set(ALL_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${UPPER_BUILD_TYPE}}")

message(STATUS "CXX_FLAGS: ${ALL_CXX_FLAGS}")

if(NOT CAF_ROOT_DIR)
  set(CAF_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/actor-framework/build/")
endif()
find_package(CAF COMPONENTS core io)
if (CAF_FOUND)
  include_directories(${CAF_INCLUDE_DIRS})
else ()
  message(FATAL_ERROR "Could not find CAF. If you do not have CAF with UDP "
                      "capabilities installed, use the script 'setup.sh' "
                      "to download and build the related branch 'topic/udp'.")
endif ()

include_directories(. include)

set(UDP_SOURCES
  src/udp_brokers.cpp
)
set(TCP_SOURCES
  src/tcp_brokers.cpp
)
set(ACTOR_SOURCES
  src/actors.cpp
)
file(GLOB_RECURSE HEADERS "include/*.hpp")

add_executable(udp_brokers
  ${UDP_SOURCES}
  ${HEADERS}
)
target_link_libraries(udp_brokers
  ${CMAKE_DL_LIBS}
  ${CAF_LIBRARY_CORE}
  ${CAF_LIBRARY_IO}
)

add_executable(tcp_brokers
  ${TCP_SOURCES}
  ${HEADERS}
)
target_link_libraries(tcp_brokers
  ${CMAKE_DL_LIBS}
  ${CAF_LIBRARY_CORE}
  ${CAF_LIBRARY_IO}
)

add_executable(actors
  ${ACTOR_SOURCES}
  ${HEADERS}
)
target_link_libraries(actors
  ${CMAKE_DL_LIBS}
  ${CAF_LIBRARY_CORE}
  ${CAF_LIBRARY_IO}
)
