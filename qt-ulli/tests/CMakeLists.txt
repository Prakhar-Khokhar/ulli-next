# tests/CMakeLists.txt
function(add_ulli_test name)
    qt_add_executable(test_${name} test_${name}.cpp)
    target_link_libraries(test_${name} PRIVATE ulli-core Qt6::Test)
    add_test(NAME ${name} COMMAND test_${name})
endfunction()

add_ulli_test(result)
add_ulli_test(catalog)
add_ulli_test(plan)
