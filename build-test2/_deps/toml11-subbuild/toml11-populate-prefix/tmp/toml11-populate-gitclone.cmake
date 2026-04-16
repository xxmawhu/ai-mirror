# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

if(EXISTS "/mnt/beegfs_data/usr/maxx/ai/ai-mirror/build-test2/_deps/toml11-subbuild/toml11-populate-prefix/src/toml11-populate-stamp/toml11-populate-gitclone-lastrun.txt" AND EXISTS "/mnt/beegfs_data/usr/maxx/ai/ai-mirror/build-test2/_deps/toml11-subbuild/toml11-populate-prefix/src/toml11-populate-stamp/toml11-populate-gitinfo.txt" AND
  "/mnt/beegfs_data/usr/maxx/ai/ai-mirror/build-test2/_deps/toml11-subbuild/toml11-populate-prefix/src/toml11-populate-stamp/toml11-populate-gitclone-lastrun.txt" IS_NEWER_THAN "/mnt/beegfs_data/usr/maxx/ai/ai-mirror/build-test2/_deps/toml11-subbuild/toml11-populate-prefix/src/toml11-populate-stamp/toml11-populate-gitinfo.txt")
  message(STATUS
    "Avoiding repeated git clone, stamp file is up to date: "
    "'/mnt/beegfs_data/usr/maxx/ai/ai-mirror/build-test2/_deps/toml11-subbuild/toml11-populate-prefix/src/toml11-populate-stamp/toml11-populate-gitclone-lastrun.txt'"
  )
  return()
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E rm -rf "/mnt/beegfs_data/usr/maxx_ai/ai/ai-mirror/build-test2/_deps/toml11-src"
  RESULT_VARIABLE error_code
)
if(error_code)
  message(FATAL_ERROR "Failed to remove directory: '/mnt/beegfs_data/usr/maxx_ai/ai/ai-mirror/build-test2/_deps/toml11-src'")
endif()

# try the clone 3 times in case there is an odd git clone issue
set(error_code 1)
set(number_of_tries 0)
while(error_code AND number_of_tries LESS 3)
  execute_process(
    COMMAND "/usr/bin/git"
            clone --no-checkout --config "advice.detachedHead=false" "https://github.com/ToruNiia/toml11.git" "toml11-src"
    WORKING_DIRECTORY "/mnt/beegfs_data/usr/maxx_ai/ai/ai-mirror/build-test2/_deps"
    RESULT_VARIABLE error_code
  )
  math(EXPR number_of_tries "${number_of_tries} + 1")
endwhile()
if(number_of_tries GREATER 1)
  message(STATUS "Had to git clone more than once: ${number_of_tries} times.")
endif()
if(error_code)
  message(FATAL_ERROR "Failed to clone repository: 'https://github.com/ToruNiia/toml11.git'")
endif()

execute_process(
  COMMAND "/usr/bin/git"
          checkout "v4.2.0" --
  WORKING_DIRECTORY "/mnt/beegfs_data/usr/maxx_ai/ai/ai-mirror/build-test2/_deps/toml11-src"
  RESULT_VARIABLE error_code
)
if(error_code)
  message(FATAL_ERROR "Failed to checkout tag: 'v4.2.0'")
endif()

set(init_submodules TRUE)
if(init_submodules)
  execute_process(
    COMMAND "/usr/bin/git" 
            submodule update --recursive --init 
    WORKING_DIRECTORY "/mnt/beegfs_data/usr/maxx_ai/ai/ai-mirror/build-test2/_deps/toml11-src"
    RESULT_VARIABLE error_code
  )
endif()
if(error_code)
  message(FATAL_ERROR "Failed to update submodules in: '/mnt/beegfs_data/usr/maxx_ai/ai/ai-mirror/build-test2/_deps/toml11-src'")
endif()

# Complete success, update the script-last-run stamp file:
#
execute_process(
  COMMAND ${CMAKE_COMMAND} -E copy "/mnt/beegfs_data/usr/maxx/ai/ai-mirror/build-test2/_deps/toml11-subbuild/toml11-populate-prefix/src/toml11-populate-stamp/toml11-populate-gitinfo.txt" "/mnt/beegfs_data/usr/maxx/ai/ai-mirror/build-test2/_deps/toml11-subbuild/toml11-populate-prefix/src/toml11-populate-stamp/toml11-populate-gitclone-lastrun.txt"
  RESULT_VARIABLE error_code
)
if(error_code)
  message(FATAL_ERROR "Failed to copy script-last-run stamp file: '/mnt/beegfs_data/usr/maxx/ai/ai-mirror/build-test2/_deps/toml11-subbuild/toml11-populate-prefix/src/toml11-populate-stamp/toml11-populate-gitclone-lastrun.txt'")
endif()
