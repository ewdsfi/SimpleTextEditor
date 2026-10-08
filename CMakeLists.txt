cmake_minimum_required(VERSION 3.16)
project(SimpleTextEditor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源码含中文，MSVC 需要显式指定 UTF-8
if(MSVC)
    add_compile_options(/utf-8)
endif()

add_executable(SimpleEditor
    main.cpp
    TextBuffer.h
    TextBuffer.cpp
    Document.h
    Document.cpp
)
