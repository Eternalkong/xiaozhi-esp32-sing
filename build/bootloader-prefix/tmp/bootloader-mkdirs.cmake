# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/skylight9/esp/v5.5/esp-idf/components/bootloader/subproject"
  "/home/skylight9/xiaozhi_ori/xiaozhi-esp32-music/build/bootloader"
  "/home/skylight9/xiaozhi_ori/xiaozhi-esp32-music/build/bootloader-prefix"
  "/home/skylight9/xiaozhi_ori/xiaozhi-esp32-music/build/bootloader-prefix/tmp"
  "/home/skylight9/xiaozhi_ori/xiaozhi-esp32-music/build/bootloader-prefix/src/bootloader-stamp"
  "/home/skylight9/xiaozhi_ori/xiaozhi-esp32-music/build/bootloader-prefix/src"
  "/home/skylight9/xiaozhi_ori/xiaozhi-esp32-music/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/skylight9/xiaozhi_ori/xiaozhi-esp32-music/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/skylight9/xiaozhi_ori/xiaozhi-esp32-music/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
