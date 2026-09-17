# tests/CMakeLists.txt
function(add_ulli_test name)
    qt_add_executable(test_${name} test_${name}.cpp)
    target_link_libraries(test_${name} PRIVATE ulli-core Qt6::Test)
    add_test(NAME ${name} COMMAND test_${name})
endfunction()

add_ulli_test(result)
add_ulli_test(catalog)
add_ulli_test(plan)
add_ulli_test(engine)
add_ulli_test(allocation)
add_ulli_test(boothandoff)
# sha256 test only built on Windows where Sha256 implementation exists
if (WIN32)
    add_ulli_test(sha256)
    add_ulli_test(copyfiles)
endif()
