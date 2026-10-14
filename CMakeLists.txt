cmake_minimum_required(VERSION 3.16)
project(SimpleTextEditor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)



set(EDITOR_CORE
    TextBuffer.h
    TextBuffer.cpp
    Document.h
    Document.cpp
    LineMap.h
    LineMap.cpp
)

set(CMAKE_AUTOMOC ON)

add_executable(SimpleEditor
    main.cpp
    EditorView.h
    EditorView.cpp
    EditorWindow.h
    EditorWindow.cpp
    ${EDITOR_CORE}
)


target_link_libraries(SimpleEditor PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)

target_include_directories(SimpleEditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
