add_executable(probe_validate probe_validate.cpp)
target_link_libraries(probe_validate PRIVATE docx_core)

add_executable(probe_rev probe_rev.cpp)
target_link_libraries(probe_rev PRIVATE docx_core)

add_executable(revisions_corpus revisions_corpus.cpp)
target_link_libraries(revisions_corpus PRIVATE docx_core)
add_test(NAME revisions_corpus COMMAND revisions_corpus ${CMAKE_SOURCE_DIR}/tests/corpus/oxpt/RP)
