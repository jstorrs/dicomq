cmake_minimum_required(VERSION 3.16)
project(storescp-plus VERSION 0.1.0 LANGUAGES CXX)

find_package(DCMTK 3.6.8 REQUIRED CONFIG)

add_executable(storescp+ src/storescp+.cc)
target_compile_definitions(storescp+ PRIVATE STORESCP_PLUS_VERSION="${PROJECT_VERSION}")
target_link_libraries(storescp+ PRIVATE DCMTK::dcmnet DCMTK::dcmtls)

include(GNUInstallDirs)
install(TARGETS storescp+ RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

add_subdirectory(dicomq)
