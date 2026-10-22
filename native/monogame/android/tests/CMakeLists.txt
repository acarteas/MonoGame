cmake_minimum_required(VERSION 3.22)
project(MonoGameAndroidHostTests LANGUAGES C CXX)

# Configure with -B pointing outside the checkout. This target compiles the
# production Android host directly, without Android SDK or a .NET runtime.
find_package(Threads REQUIRED)

# Build the pinned SDL event queue in this external test tree. No system SDL or
# developer desktop dependency build is reused, and no video/audio device opens.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
foreach(subsystem AUDIO VIDEO JOYSTICK HAPTIC SENSOR POWER)
    set(SDL_${subsystem} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../external/sdl2/sdl" pinned-sdl)

enable_testing()
add_executable(android_host_tests ../host.cpp host_tests.cpp header_c.c)
target_compile_features(android_host_tests PRIVATE cxx_std_17 c_std_11)
target_compile_definitions(android_host_tests PRIVATE MGP_ANDROID_HOST_TEST)
target_include_directories(android_host_tests PRIVATE ../../include)
target_compile_options(android_host_tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(android_host_tests PRIVATE Threads::Threads SDL2::SDL2-static)
add_test(NAME android_host_lifecycle COMMAND android_host_tests)
set_tests_properties(android_host_lifecycle PROPERTIES TIMEOUT 30)

add_executable(android_session_reuse_tests ../host.cpp session_reuse_tests.cpp)
target_compile_features(android_session_reuse_tests PRIVATE cxx_std_17)
target_compile_definitions(android_session_reuse_tests PRIVATE MGP_ANDROID_HOST_TEST)
target_include_directories(android_session_reuse_tests PRIVATE ../../include)
target_compile_options(android_session_reuse_tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(android_session_reuse_tests PRIVATE Threads::Threads SDL2::SDL2-static)
target_link_options(android_session_reuse_tests PRIVATE "-Wl,--wrap=SDL_Quit")
add_test(NAME android_session_reuse COMMAND android_session_reuse_tests)
set_tests_properties(android_session_reuse PROPERTIES TIMEOUT 30)
