# tests find the corpus through this define, wherever the build tree is
add_compile_definitions(DOCX_CORPUS="${CMAKE_SOURCE_DIR}/tests/corpus")

add_executable(probe_validate probe_validate.cpp)
target_link_libraries(probe_validate PRIVATE docx_core)

add_executable(probe_rev probe_rev.cpp)
target_link_libraries(probe_rev PRIVATE docx_core)

add_executable(edit_test edit_test.cpp)
target_link_libraries(edit_test PRIVATE docx_core)
add_test(NAME edit_test COMMAND edit_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(comments_test comments_test.cpp)
target_link_libraries(comments_test PRIVATE docx_core)
add_test(NAME comments_test COMMAND comments_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(structure_test structure_test.cpp)
target_link_libraries(structure_test PRIVATE docx_core)
add_test(NAME structure_test COMMAND structure_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(docops_test docops_test.cpp)
target_link_libraries(docops_test PRIVATE docx_core)
add_test(NAME docops_test COMMAND docops_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(robustness robustness.cpp)
target_link_libraries(robustness PRIVATE docx_core)
add_test(NAME robustness COMMAND robustness ${CMAKE_SOURCE_DIR}/tests/corpus/oxpt)

add_executable(revisions_corpus revisions_corpus.cpp)
target_link_libraries(revisions_corpus PRIVATE docx_core)
add_test(NAME revisions_corpus COMMAND revisions_corpus ${CMAKE_SOURCE_DIR}/tests/corpus/oxpt/RP)

add_executable(probe_perf probe_perf.cpp)
target_link_libraries(probe_perf PRIVATE docx_core)
