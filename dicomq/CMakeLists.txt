# dicomq — qmail-inspired DICOM store-and-forward suite. See DESIGN.md.

add_library(dicomq-common STATIC
  src/common/spool.cc
  src/common/envelope.cc
  src/common/message.cc
  src/common/profile.cc
)
target_include_directories(dicomq-common PUBLIC src)
target_compile_features(dicomq-common PUBLIC cxx_std_17)

foreach(prog recv send local remote clean inject queue super)
  add_executable(dicomq-${prog} src/${prog}/main.cc)
  target_link_libraries(dicomq-${prog} PRIVATE dicomq-common)
endforeach()

# Only the edge programs speak DICOM; the queue core must stay free of
# DCMTK so it can be tested without it.
target_link_libraries(dicomq-recv PRIVATE DCMTK::dcmnet DCMTK::dcmtls)
target_link_libraries(dicomq-remote PRIVATE DCMTK::dcmnet DCMTK::dcmtls
                                            DCMTK::dcmjpeg DCMTK::dcmjpls)
target_link_libraries(dicomq-inject PRIVATE DCMTK::dcmdata)

foreach(prog recv send local remote clean inject queue super)
  install(TARGETS dicomq-${prog} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endforeach()
