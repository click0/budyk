# SPDX-License-Identifier: BSD-3-Clause
# cmake/docs.cmake — `cmake --build build --target docs`: the API reference
# of the headers in src/ (Doxygen, HTML in build/docs/html/index.html).
# The /// comments above a declaration and the ///< after a member are
# its text. Any Doxygen warning fails the target; CI builds it. Without
# doxygen there is no `docs` target, and nothing else changes.

find_package(Doxygen QUIET)
if(NOT DOXYGEN_FOUND)
    return()
endif()

set(DOXYGEN_PROJECT_NAME "budyk")
set(DOXYGEN_PROJECT_BRIEF "Lightweight server monitoring with adaptive collection")
set(DOXYGEN_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/docs")
set(DOXYGEN_STRIP_FROM_PATH "${PROJECT_SOURCE_DIR}/src")
set(DOXYGEN_STRIP_FROM_INC_PATH "${PROJECT_SOURCE_DIR}/src")
set(DOXYGEN_FILE_PATTERNS "*.h")
set(DOXYGEN_EXTRACT_ALL YES)
set(DOXYGEN_EXTRACT_STATIC YES)
# No JAVADOC_AUTOBRIEF: it ends the first sentence at a dot inside a
# `code span` (`collection.<level>.metrics`), then warns about the span.
set(DOXYGEN_SORT_MEMBER_DOCS NO)
# Both platforms' declarations, whatever this host is.
set(DOXYGEN_PREDEFINED "BUDYK_LINUX" "BUDYK_FREEBSD" "__cplusplus")
set(DOXYGEN_HAVE_DOT NO)
set(DOXYGEN_QUIET YES)
set(DOXYGEN_WARN_IF_UNDOCUMENTED NO)
set(DOXYGEN_WARN_AS_ERROR FAIL_ON_WARNINGS)

doxygen_add_docs(docs "${PROJECT_SOURCE_DIR}/src"
                 COMMENT "API reference (Doxygen) in ${PROJECT_BINARY_DIR}/docs/html")
