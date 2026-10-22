cmake_minimum_required(VERSION 3.22)
project(MonoGameAndroidHostTests LANGUAGES C CXX)

# Configure with -B pointing outside the checkout. This target compiles the
# production Android host directly, without SDL, Android SDK or a .NET runtime.
find_package(Threads REQUIRED)
enable_testing()
add_executable(android_host_tests ../host.cpp host_tests.cpp header_c.c)
target_compile_features(android_host_tests PRIVATE cxx_std_17 c_std_11)
target_compile_definitions(android_host_tests PRIVATE MGP_ANDROID_HOST_TEST)
target_include_directories(android_host_tests PRIVATE ../../include)
target_compile_options(android_host_tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(android_host_tests PRIVATE Threads::Threads)
add_test(NAME android_host_lifecycle COMMAND android_host_tests)
set_tests_properties(android_host_lifecycle PROPERTIES TIMEOUT 30)
