# Do not use lofar_add_test, because libaoflaggerstep should not be linked in.
add_executable(tAOFlaggerStep tAOFlaggerStep.cc)

target_link_libraries (tAOFlaggerStep ${_libs}  ${LOFAR_EXTRA_LIBRARIES})

add_test (tAOFlaggerStep ${CMAKE_SOURCE_DIR}/DP3/DPPP_AOFlag/test/tAOFlaggerStep.sh)
