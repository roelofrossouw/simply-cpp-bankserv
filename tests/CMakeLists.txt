set(SC_TEST_LINK_LIBRARIES sc-bankserv)

# Made-up load reports in the bank's layout (tests/data), and the reader's edge cases
add_sc_test(load_report LABELS unit)
target_compile_definitions(test-load_report PRIVATE SC_BANKSERV_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
