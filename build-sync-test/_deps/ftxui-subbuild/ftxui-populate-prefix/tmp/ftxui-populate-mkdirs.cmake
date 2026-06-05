# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/ftxui-src"
  "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/ftxui-build"
  "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/ftxui-subbuild/ftxui-populate-prefix"
  "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/ftxui-subbuild/ftxui-populate-prefix/tmp"
  "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/ftxui-subbuild/ftxui-populate-prefix/src/ftxui-populate-stamp"
  "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/ftxui-subbuild/ftxui-populate-prefix/src"
  "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/ftxui-subbuild/ftxui-populate-prefix/src/ftxui-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/ftxui-subbuild/ftxui-populate-prefix/src/ftxui-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/_deps/ftxui-subbuild/ftxui-populate-prefix/src/ftxui-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
