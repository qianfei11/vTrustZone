cmake_minimum_required(VERSION 3.5)

project(qemu_tzc_core_isolation C)

set(SRC host/main.c)

add_executable(${PROJECT_NAME} ${SRC})

install(TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
