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

# Emit build/compile_commands.json so clang-tidy and clangd know each
# file's flags (see .clang-tidy). Warnings are dicomq's first-line linter;
# kept advisory (no -Werror) so a new compiler's warnings never block a
# build, only inform.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra)

add_library(dicomq-common STATIC
  src/common/spool.cc
  src/common/kvfile.cc
  src/common/message.cc
  src/common/profile.cc
)
target_include_directories(dicomq-common PUBLIC src)
target_compile_features(dicomq-common PUBLIC cxx_std_17)

# DICOM-aware common helpers (file-meta reading). Kept apart from
# dicomq-common so the DICOM-free programs need not link DCMTK; only the
# programs that read a queued object's header link this. dcmdata is PUBLIC
# so it propagates to those consumers.
add_library(dicomq-dcm STATIC src/common/dcm.cc)
target_include_directories(dicomq-dcm PUBLIC src)
target_compile_features(dicomq-dcm PUBLIC cxx_std_17)
target_link_libraries(dicomq-dcm PUBLIC DCMTK::dcmdata)

# Shared BCP 195 TLS setup. Its own library so only dicomq-recv and
# dicomq-remote (the two programs that speak TLS) pull in DCMTK::dcmtls;
# dcmtls is PUBLIC so the link requirement propagates to those consumers.
add_library(dicomq-tls STATIC src/common/tls.cc)
target_include_directories(dicomq-tls PUBLIC src)
target_compile_features(dicomq-tls PUBLIC cxx_std_17)
target_link_libraries(dicomq-tls PUBLIC DCMTK::dcmtls PRIVATE dicomq-common)

foreach(prog recv send local remote clean inject queue ctl)
  add_executable(dicomq-${prog} src/${prog}/main.cc)
  target_link_libraries(dicomq-${prog} PRIVATE dicomq-common)
endforeach()

# dicomq-send, -local and -clean speak no DICOM (routing is by directory).
# The rest are DICOM-aware and link dicomq-dcm for the shared file-meta
# helpers: recv stamps and accepts objects over the network and writes them
# with saveAsReceived(); inject submits them the same way; remote forwards
# them; and remote/inject/ctl/queue read the file-meta header to recover a
# routed message's routing fields (no sidecar carries them anymore).
target_link_libraries(dicomq-recv PRIVATE dicomq-dcm dicomq-tls DCMTK::dcmnet)
target_link_libraries(dicomq-remote PRIVATE dicomq-dcm dicomq-tls DCMTK::dcmnet
                      DCMTK::dcmjpeg DCMTK::dcmjpls)
target_link_libraries(dicomq-inject PRIVATE dicomq-dcm)
target_link_libraries(dicomq-ctl PRIVATE dicomq-dcm)
target_link_libraries(dicomq-queue PRIVATE dicomq-dcm)

foreach(prog recv send local remote clean inject queue ctl)
  install(TARGETS dicomq-${prog} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endforeach()

# Unit tests for the pure common helpers — paths the network integration
# tests cannot reach (e.g. lossy classification of video syntaxes).
add_executable(dicomq-unit-profile test/unit-profile.cc)
target_link_libraries(dicomq-unit-profile PRIVATE dicomq-common)
add_test(NAME dicomq-unit-profile COMMAND dicomq-unit-profile)

# Filesystem-helper edge cases (EEXIST-wrong-inode, non-directory paths,
# absent vs required config) — pure, no DICOM networking required.
add_executable(dicomq-unit-spool test/unit-spool.cc)
target_link_libraries(dicomq-unit-spool PRIVATE dicomq-common)
add_test(NAME dicomq-unit-spool COMMAND dicomq-unit-spool)

add_test(NAME dicomq-integration
  COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/test/run-tests.sh"
               "$<TARGET_FILE_DIR:dicomq-send>")
set_tests_properties(dicomq-integration PROPERTIES TIMEOUT 300)
