# Read by cpack itself (not by cmake) for every package it builds.
#
# CMake 3.21 to 3.31 decide whether rpmbuild supports weak dependencies by
# running ${RPM_EXECUTABLE} --suggests, but never set RPM_EXECUTABLE. The test
# always fails and CPACK_RPM_PACKAGE_SUGGESTS is dropped without an error
# (CMake issue #22350, fixed in 4.0). Finding rpm here makes the test work.
if(CPACK_GENERATOR STREQUAL "RPM" AND NOT RPM_EXECUTABLE)
    find_program(RPM_EXECUTABLE rpm)
endif()
