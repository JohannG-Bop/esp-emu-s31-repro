cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Only build what main needs (no unrelated components/submodules).
set(COMPONENTS main)
project(s31_bluedroid_softap)
# S31's upstream HAL aggregate initializers trigger this warning.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=missing-field-initializers" APPEND)
