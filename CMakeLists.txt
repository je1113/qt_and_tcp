cmake_minimum_required(VERSION 3.16)
project(qt_and_tcp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt의 moc(시그널/슬롯용 코드 생성기)를 자동으로 실행한다. 나중에 Q_OBJECT를 쓸 때 필요하다.
set(CMAKE_AUTOMOC ON)

find_package(Qt5 REQUIRED COMPONENTS Widgets Network)

add_executable(qt_and_tcp
    main.cpp
    mainwindow.cpp
    mainwindow.h
)
target_link_libraries(qt_and_tcp PRIVATE Qt5::Widgets Qt5::Network)
