cmake_minimum_required(VERSION 3.21)

project(mam VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT APPLE)
    message(FATAL_ERROR "MAM currently requires macOS and the CoreAudio SDK")
endif()

option(MAM_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(MAM_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MAM_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(MAM_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(MAM_ENABLE_LTO "Enable Link-Time Optimization" OFF)
option(MAM_ENABLE_PYTHON_TESTS "Run optional Python worker and native integration tests" OFF)
set(MAM_PYTHON_EXECUTABLE "" CACHE FILEPATH "Interpreter for the optional offline Python tools")
if(NOT MAM_PYTHON_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.venv/bin/python")
    get_filename_component(MAM_PYTHON_EXECUTABLE "${CMAKE_CURRENT_SOURCE_DIR}/.venv/bin/python" ABSOLUTE)
endif()
option(MAM_USE_JSON_SCHEMA "Enable a real JSON Schema validator (not yet integrated)" OFF)
if(MAM_USE_JSON_SCHEMA)
    message(FATAL_ERROR "A native JSON Schema validator is not integrated. Use MAM_USE_JSON_SCHEMA=OFF; --schema-strict will explicitly fail until a real validator is integrated.")
endif()

include(CTest)
include(cmake/MamOptions.cmake)
find_package(Threads REQUIRED)

add_library(mam_dependencies INTERFACE)
target_include_directories(mam_dependencies SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/vendor)


add_library(mam_core STATIC
    src/core/Instrument.hpp
    src/core/Node.hpp
    src/core/MixerNode.hpp
    src/core/GraphConfig.hpp
    src/core/GraphConfig.cpp
    src/core/RationalFrameClock.cpp
    src/offline/TransportGenerator.cpp
    src/core/Graph.hpp
    src/core/AudioBuffer.hpp
    src/core/Command.hpp
    src/core/JobPool.hpp
    src/core/ParamIds.hpp
    src/core/ParameterRegistry.hpp
    src/core/ParamMap.hpp
    src/core/TransportNode.hpp
    src/core/CompressorNode.hpp
    src/core/ReverbNode.hpp
    src/core/WiretapNode.hpp
    src/core/SchemaValidate.hpp
    src/core/SchemaValidate.cpp
    src/session/SessionSpec.hpp
    src/session/SessionRuntime.hpp
    src/realtime/RealtimeSessionRenderer.hpp
)

target_link_libraries(mam_core PUBLIC mam_dependencies Threads::Threads)

add_library(mam_dsp STATIC
    src/instruments/kick/KickSynth.hpp
    src/instruments/kick/KickSynth.cpp
    src/instruments/clap/ClapSynth.hpp
    src/instruments/clap/ClapSynth.cpp
)

add_library(mam_io STATIC
    src/io/AudioFileWriter.hpp
    src/io/AudioFileWriter.cpp
    src/io/NativeAudio.hpp
    src/io/NativeAudio.cpp
)

add_library(mam_render INTERFACE)
add_library(mam_nodes STATIC src/core/NodeFactory.hpp src/core/NodeFactory.cpp)
target_link_libraries(mam_nodes PUBLIC mam_core PRIVATE mam_dsp mam_io)
add_library(mam_session STATIC src/session/RackTimeline.cpp)
target_link_libraries(mam_session PUBLIC mam_core)
add_library(mam_app STATIC src/app/CliOptions.cpp src/app/SessionPlayback.cpp)
target_link_libraries(mam_app PUBLIC mam_render mam_session)
add_executable(mam
    src/main.cpp
    src/tools/BuildInfo.cpp
    src/tools/PythonTools.cpp
    src/offline/StemRenderer.cpp
)
add_custom_target(mam_build_identity
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/BuildIdentity.hpp
        -DVERSION=${PROJECT_VERSION}
        -DCOMPILER_ID=${CMAKE_CXX_COMPILER_ID}
        -DCOMPILER_VERSION=${CMAKE_CXX_COMPILER_VERSION}
        -DBUILD_TYPE=$<CONFIG>
        -DASAN=${MAM_ENABLE_ASAN} -DUBSAN=${MAM_ENABLE_UBSAN}
        -DTSAN=${MAM_ENABLE_TSAN} -DLTO=${MAM_ENABLE_LTO}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateBuildIdentity.cmake
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/BuildIdentity.hpp
    VERBATIM)
add_dependencies(mam mam_build_identity)
target_include_directories(mam PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(mam PRIVATE
    MAM_DEFAULT_PYTHON="${MAM_PYTHON_EXECUTABLE}"
    MAM_DEFAULT_PYTHON_TOOLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/python"
)

target_link_libraries(mam_render INTERFACE mam_core mam_dsp mam_io mam_nodes)
target_link_libraries(mam PUBLIC mam_render mam_app)

target_compile_definitions(mam PRIVATE MACOSX)
# Consumers of the header-based I/O API (including tests) need these too.
target_link_libraries(mam_io PUBLIC
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreAudio"
        "-framework CoreServices"
        "-framework CoreFoundation"
)

add_executable(gen_params tools/gen_params.cpp)
target_link_libraries(gen_params PRIVATE mam_core)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/docs/ParamTables.md
    COMMAND ${CMAKE_COMMAND}
        -DGENERATOR=$<TARGET_FILE:gen_params>
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/docs/ParamTables.md
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateParamDocs.cmake
    DEPENDS gen_params src/core/ParamMap.hpp cmake/GenerateParamDocs.cmake
    VERBATIM
    COMMENT "Generating parameter docs in the build directory"
)
add_custom_target(docs DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/docs/ParamTables.md)

foreach(tgt IN ITEMS mam_core mam_dsp mam_io mam_nodes mam_session mam_app mam gen_params)
    mam_configure_target(${tgt})
endforeach()

if(BUILD_TESTING)
    add_subdirectory(vendor/doctest)
    add_subdirectory(tests)
endif()
