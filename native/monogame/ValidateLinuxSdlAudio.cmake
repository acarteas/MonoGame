cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED SDL_BUILD_DIR OR SDL_BUILD_DIR STREQUAL "")
    message(FATAL_ERROR "Set SDL_BUILD_DIR to the configured Linux SDL2 build directory.")
endif()

file(GLOB configuration_headers
    "${SDL_BUILD_DIR}/include-config-*/SDL2/SDL_config.h"
    "${SDL_BUILD_DIR}/include/SDL2/SDL_config.h"
)
list(LENGTH configuration_headers header_count)
if(NOT header_count EQUAL 1)
    message(FATAL_ERROR
        "Expected one generated SDL_config.h in ${SDL_BUILD_DIR}, found ${header_count}. "
        "Configure bundled SDL2 first; see REQUIREMENTS.md.")
endif()

list(GET configuration_headers 0 configuration_header)
file(READ "${configuration_header}" configuration)
set(missing_backends "")
foreach(backend IN ITEMS ALSA PULSEAUDIO)
    if(NOT configuration MATCHES "(^|[\r\n])[ \t]*#[ \t]*define[ \t]+SDL_AUDIO_DRIVER_${backend}[ \t]+1([ \t\r\n]|$)")
        list(APPEND missing_backends "${backend}")
    endif()
endforeach()

if(missing_backends)
    list(JOIN missing_backends ", " missing_names)
    message(FATAL_ERROR
        "Bundled SDL2 is missing required Linux audio backends: ${missing_names}. "
        "Install pkg-config and ALSA/PulseAudio development packages "
        "(Debian/Ubuntu: libasound2-dev libpulse-dev; Fedora: alsa-lib-devel pulseaudio-libs-devel; "
        "openSUSE: alsa-devel libpulse-devel), then reconfigure and rebuild native dependencies. "
        "SDL_ALSA=ON and SDL_PULSEAUDIO=ON alone do not guarantee compiled support. "
        "PipeWire is optional. See REQUIREMENTS.md.")
endif()

message(STATUS "Bundled SDL2 Linux audio backends verified: ALSA and PulseAudio")
