# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/mnt/beegfs_data/usr/maxx_ai/ai/ai-mirror/build-test2/_deps/cli11-src"
  "/mnt/beegfs_data/usr/maxx_ai/ai/ai-mirror/build-test2/_deps/cli11-build"
  "/mnt/beegfs_data/usr/maxx/ai/ai-mirror/build-test2/_deps/cli11-subbuild/cli11-populate-prefix"
  "/mnt/beegfs_data/usr/maxx/ai/ai-mirror/build-test2/_deps/cli11-subbuild/cli11-populate-prefix/tmp"
  "/mnt/beegfs_data/usr/maxx/ai/ai-mirror/build-test2/_deps/cli11-subbuild/cli11-populate-prefix/src/cli11-populate-stamp"
  "/mnt/beegfs_data/usr/maxx/ai/ai-mirror/build-test2/_deps/cli11-subbuild/cli11-populate-prefix/src"
  "/mnt/beegfs_data/usr/maxx/ai/ai-mirror/build-test2/_deps/cli11-subbuild/cli11-populate-prefix/src/cli11-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/mnt/beegfs_data/usr/maxx/ai/ai-mirror/build-test2/_deps/cli11-subbuild/cli11-populate-prefix/src/cli11-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/mnt/beegfs_data/usr/maxx/ai/ai-mirror/build-test2/_deps/cli11-subbuild/cli11-populate-prefix/src/cli11-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
