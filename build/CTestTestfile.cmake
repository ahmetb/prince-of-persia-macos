# CMake generated Testfile for 
# Source directory: /Users/ahmet.balkan/junk/prince.exe
# Build directory: /Users/ahmet.balkan/junk/prince.exe/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test("save_rejection_tests" "/Users/ahmet.balkan/junk/prince.exe/build/save_rejection_tests")
set_tests_properties("save_rejection_tests" PROPERTIES  _BACKTRACE_TRIPLES "/Users/ahmet.balkan/junk/prince.exe/CMakeLists.txt;195;add_test;/Users/ahmet.balkan/junk/prince.exe/CMakeLists.txt;198;add_pop_test;/Users/ahmet.balkan/junk/prince.exe/CMakeLists.txt;0;")
subdirs("_deps/sdl2-build")
subdirs("_deps/sdl2_image-build")
subdirs("src")
