# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "RelWithDebInfo")
  file(REMOVE_RECURSE
  "CMakeFiles/ulli-core_autogen.dir/AutogenUsed.txt"
  "CMakeFiles/ulli-core_autogen.dir/ParseCache.txt"
  "CMakeFiles/ulli-qt_autogen.dir/AutogenUsed.txt"
  "CMakeFiles/ulli-qt_autogen.dir/ParseCache.txt"
  "tests/CMakeFiles/test_catalog_autogen.dir/AutogenUsed.txt"
  "tests/CMakeFiles/test_catalog_autogen.dir/ParseCache.txt"
  "tests/CMakeFiles/test_engine_autogen.dir/AutogenUsed.txt"
  "tests/CMakeFiles/test_engine_autogen.dir/ParseCache.txt"
  "tests/CMakeFiles/test_plan_autogen.dir/AutogenUsed.txt"
  "tests/CMakeFiles/test_plan_autogen.dir/ParseCache.txt"
  "tests/CMakeFiles/test_result_autogen.dir/AutogenUsed.txt"
  "tests/CMakeFiles/test_result_autogen.dir/ParseCache.txt"
  "tests/test_catalog_autogen"
  "tests/test_engine_autogen"
  "tests/test_plan_autogen"
  "tests/test_result_autogen"
  "ulli-core_autogen"
  "ulli-qt_autogen"
  )
endif()
