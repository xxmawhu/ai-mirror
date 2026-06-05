# CMake generated Testfile for 
# Source directory: /mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror
# Build directory: /mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[test_watch_stats]=] "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/bin/test_watch_stats")
set_tests_properties([=[test_watch_stats]=] PROPERTIES  _BACKTRACE_TRIPLES "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/CMakeLists.txt;158;add_test;/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/CMakeLists.txt;0;")
add_test([=[test_known_hosts]=] "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/build-sync-test/bin/test_known_hosts")
set_tests_properties([=[test_known_hosts]=] PROPERTIES  _BACKTRACE_TRIPLES "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/CMakeLists.txt;181;add_test;/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/github-ai-mirror/CMakeLists.txt;0;")
subdirs("_deps/cli11-build")
subdirs("_deps/toml11-build")
subdirs("_deps/spdlog-build")
subdirs("_deps/nlohmann_json-build")
subdirs("_deps/ftxui-build")
