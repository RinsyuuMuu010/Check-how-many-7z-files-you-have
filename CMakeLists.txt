cmake_minimum_required(VERSION 3.20)
project(check7z VERSION 2.0.0 LANGUAGES CXX)

add_executable(check7z check7z.cpp)

target_compile_features(check7z PRIVATE cxx_std_17)
set_target_properties(check7z PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(check7z PRIVATE /utf-8 /W4 /permissive- /EHsc)
    # 静态链接 CRT：生成的 exe 可以直接拷给别人用
    set_property(TARGET check7z PROPERTY MSVC_RUNTIME_LIBRARY
                 "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(check7z PRIVATE -Wall -Wextra -O2)
endif()

target_link_libraries(check7z PRIVATE advapi32 shell32 version)

if(MINGW AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
    target_link_libraries(check7z PRIVATE stdc++fs)
endif()
