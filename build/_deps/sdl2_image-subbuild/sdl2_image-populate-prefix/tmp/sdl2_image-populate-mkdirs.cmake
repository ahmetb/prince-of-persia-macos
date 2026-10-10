# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/ahmet.balkan/junk/prince.exe/build/_deps/sdl2_image-src")
  file(MAKE_DIRECTORY "/Users/ahmet.balkan/junk/prince.exe/build/_deps/sdl2_image-src")
endif()
file(MAKE_DIRECTORY
  "/Users/ahmet.balkan/junk/prince.exe/build/_deps/sdl2_image-build"
  "/Users/ahmet.balkan/junk/prince.exe/build/_deps/sdl2_image-subbuild/sdl2_image-populate-prefix"
  "/Users/ahmet.balkan/junk/prince.exe/build/_deps/sdl2_image-subbuild/sdl2_image-populate-prefix/tmp"
  "/Users/ahmet.balkan/junk/prince.exe/build/_deps/sdl2_image-subbuild/sdl2_image-populate-prefix/src/sdl2_image-populate-stamp"
  "/Users/ahmet.balkan/junk/prince.exe/build/_deps/sdl2_image-subbuild/sdl2_image-populate-prefix/src"
  "/Users/ahmet.balkan/junk/prince.exe/build/_deps/sdl2_image-subbuild/sdl2_image-populate-prefix/src/sdl2_image-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/ahmet.balkan/junk/prince.exe/build/_deps/sdl2_image-subbuild/sdl2_image-populate-prefix/src/sdl2_image-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/ahmet.balkan/junk/prince.exe/build/_deps/sdl2_image-subbuild/sdl2_image-populate-prefix/src/sdl2_image-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
