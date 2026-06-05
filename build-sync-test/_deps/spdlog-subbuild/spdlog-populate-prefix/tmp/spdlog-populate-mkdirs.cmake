# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/spdlog-src"
  "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/spdlog-build"
  "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/spdlog-subbuild/spdlog-populate-prefix"
  "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/spdlog-subbuild/spdlog-populate-prefix/tmp"
  "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/spdlog-subbuild/spdlog-populate-prefix/src/spdlog-populate-stamp"
  "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/spdlog-subbuild/spdlog-populate-prefix/src"
  "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/spdlog-subbuild/spdlog-populate-prefix/src/spdlog-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/spdlog-subbuild/spdlog-populate-prefix/src/spdlog-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/spdlog-subbuild/spdlog-populate-prefix/src/spdlog-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
