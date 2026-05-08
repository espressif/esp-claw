cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rover_demo)

fatfs_create_spiflash_image(storage fatfs_image FLASH_IN_PROJECT)
