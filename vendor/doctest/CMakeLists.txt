# Pinned upstream files; no configure-time download or package-manager dependency.
add_library(mam_doctest INTERFACE)
add_library(doctest::doctest ALIAS mam_doctest)
target_include_directories(mam_doctest SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
