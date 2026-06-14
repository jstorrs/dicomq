# dicomq — qmail-inspired DICOM store-and-forward suite. See DESIGN.md.

cmake_minimum_required(VERSION 3.16)
project(dicomq VERSION 0.1.0 LANGUAGES CXX)

# 3.7.0 is the floor: it fixes CVE-2025-14607 (remote memory corruption in
# DcmByteString::makeDicomByteString, in dcmdata -- reachable from the DICOM
# parsing dicomq does), present through 3.6.9. DCMTKConfigVersion.cmake reports
# DCMTK_PACKAGE_VERSION (major.minor.build = 3.7.0), so this check resolves
# correctly against a real 3.7.0 install.
find_package(DCMTK 3.7.0 REQUIRED CONFIG)

include(GNUInstallDirs)   # provides CMAKE_INSTALL_BINDIR used by install()
enable_testing()

add_library(dicomq-common STATIC
  src/common/spool.cc
  src/common/envelope.cc
  src/common/message.cc
  src/common/profile.cc
)
target_include_directories(dicomq-common PUBLIC src)
target_compile_features(dicomq-common PUBLIC cxx_std_17)

foreach(prog recv send local remote clean inject queue ctl)
  add_executable(dicomq-${prog} src/${prog}/main.cc)
  target_link_libraries(dicomq-${prog} PRIVATE dicomq-common)
endforeach()

# Only the edge programs speak DICOM; the queue core must stay free of
# DCMTK so it can be tested without it.
target_link_libraries(dicomq-recv PRIVATE DCMTK::dcmnet DCMTK::dcmtls)
target_link_libraries(dicomq-remote PRIVATE DCMTK::dcmnet DCMTK::dcmtls
                                            DCMTK::dcmjpeg DCMTK::dcmjpls)
target_link_libraries(dicomq-inject PRIVATE DCMTK::dcmdata)

foreach(prog recv send local remote clean inject queue ctl)
  install(TARGETS dicomq-${prog} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endforeach()

# Unit tests for the pure common helpers — paths the network integration
# tests cannot reach (e.g. lossy classification of video syntaxes).
add_executable(dicomq-unit-profile test/unit-profile.cc)
target_link_libraries(dicomq-unit-profile PRIVATE dicomq-common)
add_test(NAME dicomq-unit-profile COMMAND dicomq-unit-profile)

add_test(NAME dicomq-integration
  COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/test/run-tests.sh"
               "$<TARGET_FILE_DIR:dicomq-send>")
set_tests_properties(dicomq-integration PROPERTIES TIMEOUT 300)
