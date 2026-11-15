cmake_minimum_required(VERSION 3.20)

project(AudioPlayer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF)
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

add_subdirectory(deps/red4ext.sdk)

# Silence SDK-internal warnings and treat SDK headers as system includes so
# templates instantiated in our TU don't produce noise either.
target_compile_options(RED4ext.SDK PRIVATE /w)
get_target_property(_r4_inc RED4ext.SDK INTERFACE_INCLUDE_DIRECTORIES)
if(_r4_inc)
  set_target_properties(RED4ext.SDK PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_r4_inc}")
endif()

# FMOD Core (Windows prebuilt from the FMOD Engine installer).
set(FMOD_DIR "" CACHE PATH "Root of the FMOD Core API ('api/core')")
set(_FMOD_HINTS "${FMOD_DIR}" "$ENV{FMOD_DIR}" "${CMAKE_SOURCE_DIR}/deps/fmod")
find_path(FMOD_INCLUDE_DIR
  NAMES fmod.hpp
  HINTS ${_FMOD_HINTS}
  PATH_SUFFIXES inc include
  REQUIRED)
find_library(FMOD_LIBRARY
  NAMES fmod_vc fmod
  HINTS ${_FMOD_HINTS}
  PATH_SUFFIXES lib/x64 lib
  REQUIRED)
find_file(FMOD_RUNTIME_LIBRARY
  NAMES fmod.dll
  HINTS ${_FMOD_HINTS}
  PATH_SUFFIXES lib/x64 lib)

add_library(FMOD::Core SHARED IMPORTED)
set_target_properties(FMOD::Core PROPERTIES
  IMPORTED_IMPLIB "${FMOD_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${FMOD_INCLUDE_DIR}")
if(FMOD_RUNTIME_LIBRARY)
  set_target_properties(FMOD::Core PROPERTIES IMPORTED_LOCATION "${FMOD_RUNTIME_LIBRARY}")
endif()

# Regenerate the REDscript declaration from main.cpp on every build so the
# .reds stays in sync with the registered RTTI signatures.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_reds_out "${CMAKE_SOURCE_DIR}/bin/r6/scripts/AudioPlayer/AudioPlayer.reds")
set(_reds_src "${CMAKE_SOURCE_DIR}/src/red4ext/main.cpp")
set(_reds_gen "${CMAKE_SOURCE_DIR}/scripts/gen_reds.py")
add_custom_command(
  OUTPUT "${_reds_out}"
  COMMAND ${Python3_EXECUTABLE} "${_reds_gen}" "${_reds_src}" "${_reds_out}"
  DEPENDS "${_reds_src}" "${_reds_gen}"
  COMMENT "Generating AudioPlayer.reds from main.cpp"
  VERBATIM)
add_custom_target(AudioPlayer_reds ALL DEPENDS "${_reds_out}")

add_library(AudioPlayer SHARED src/red4ext/main.cpp)
add_dependencies(AudioPlayer AudioPlayer_reds)

if(WIN32)
  target_sources(AudioPlayer PRIVATE ${CMAKE_SOURCE_DIR}/src/red4ext/version.rc)
endif()

target_link_libraries(AudioPlayer PRIVATE RED4ext::SDK FMOD::Core)

set_target_properties(AudioPlayer PROPERTIES
  OUTPUT_NAME AudioPlayer
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/red4ext/plugins/AudioPlayer)

if(FMOD_RUNTIME_LIBRARY)
  add_custom_command(TARGET AudioPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${FMOD_RUNTIME_LIBRARY}"
      "${CMAKE_SOURCE_DIR}/bin/red4ext/plugins/AudioPlayer/")
endif()
