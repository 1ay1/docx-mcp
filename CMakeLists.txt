cmake_minimum_required(VERSION 3.24)
project(docx_mcp VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(DOCX_BUILD_TESTS "Build tests" ON)
option(DOCX_STATIC "Link executables fully statically" OFF)
if(DOCX_STATIC OR MINGW)
    # MinGW: never depend on the toolchain's runtime DLLs (libc++, libunwind, libstdc++)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
        add_link_options(-static)
    endif()
endif()
if(MSVC)
    # static CRT, UTF-8 source and execution charset, standard conformance
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /bigobj /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(WIN32)
    add_compile_definitions(UNICODE _UNICODE)
endif()
if(MSVC)
    set(DOCX_WARNINGS /W3)
else()
    set(DOCX_WARNINGS -Wall -Wextra -Wno-unused-parameter)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 14)
        list(APPEND DOCX_WARNINGS -Wdangling-reference)
    endif()
endif()

# Everything is vendored or a submodule; nothing comes from the system or
# the network. mcp-cpp FetchContent's its deps, so point those at our trees.
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json CACHE PATH "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_JSONRPC_CPP ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jsonrpc-cpp CACHE PATH "" FORCE)
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)
set(JSONRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MCP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MCP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/mcp-cpp EXCLUDE_FROM_ALL)

add_library(docx_thirdparty STATIC
    third_party/pugixml/pugixml.cpp
    third_party/miniz/miniz.c)
target_include_directories(docx_thirdparty SYSTEM PUBLIC third_party/pugixml third_party/miniz)
target_compile_definitions(docx_thirdparty PUBLIC PUGIXML_HAS_LONG_LONG MINIZ_NO_ZLIB_COMPATIBLE_NAMES MINIZ_NO_STDIO)
set_target_properties(docx_thirdparty PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The generated schema tables are big; compile them once, without warnings.
add_library(docx_schema_data STATIC src/schema/schema_data.cpp)
if(MSVC)
    target_compile_options(docx_schema_data PRIVATE /O1 /w)
else()
    target_compile_options(docx_schema_data PRIVATE -O1 -w)
endif()

file(GLOB_RECURSE DOCX_CORE_SOURCES CONFIGURE_DEPENDS
    src/util/*.cpp src/opc/*.cpp src/xml/*.cpp src/schema/*.cpp src/wml/*.cpp src/mcp/session.cpp src/mcp/tools*.cpp)
list(REMOVE_ITEM DOCX_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/schema/schema_data.cpp)

# Word's template parts (blank document, built-in styles) compiled in.
file(GLOB_RECURSE DOCX_ASSETS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/*.xml)
set(DOCX_ASSETS_CPP ${CMAKE_CURRENT_BINARY_DIR}/assets_data.cpp)
set(DOCX_ASSETS_LIST ${CMAKE_CURRENT_BINARY_DIR}/assets_list.txt)
string(REPLACE ";" "\n" _assets_lines "${DOCX_ASSETS}")
file(WRITE ${DOCX_ASSETS_LIST}.tmp "${_assets_lines}")
file(COPY_FILE ${DOCX_ASSETS_LIST}.tmp ${DOCX_ASSETS_LIST} ONLY_IF_DIFFERENT)
add_custom_command(OUTPUT ${DOCX_ASSETS_CPP}
    COMMAND ${CMAKE_COMMAND} -DOUT=${DOCX_ASSETS_CPP} -DLIST=${DOCX_ASSETS_LIST}
            -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/assets -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${DOCX_ASSETS} ${DOCX_ASSETS_LIST} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake)
list(APPEND DOCX_CORE_SOURCES ${DOCX_ASSETS_CPP})
add_library(docx_core STATIC ${DOCX_CORE_SOURCES})
target_include_directories(docx_core PUBLIC src)
target_link_libraries(docx_core PUBLIC docx_thirdparty docx_schema_data nlohmann_json::nlohmann_json)
target_compile_options(docx_core PRIVATE ${DOCX_WARNINGS})

add_executable(docx-mcp src/mcp/main.cpp)
target_link_libraries(docx-mcp PRIVATE docx_core mcp::mcp)
target_compile_options(docx-mcp PRIVATE ${DOCX_WARNINGS})
target_compile_definitions(docx-mcp PRIVATE DOCX_MCP_VERSION="${PROJECT_VERSION}")
target_link_libraries(docx_core PUBLIC mcp::mcp)

if(DOCX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
