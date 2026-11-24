# Version provenance is refreshed on configure. Release packaging checks it
# against the current checkout and can require a clean tree.
set(LB_GIT_REVISION "" CACHE STRING "Source commit override for builds without .git")
find_package(Git QUIET)
set(LB_REVISION "unknown")
set(LB_DIRTY_JSON "null")
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE LB_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE LB_STATUS
    COMMAND_ERROR_IS_FATAL ANY)
  if(LB_STATUS STREQUAL "")
    set(LB_DIRTY_JSON "false")
  else()
    set(LB_DIRTY_JSON "true")
  endif()
elseif(NOT LB_GIT_REVISION STREQUAL "")
  set(LB_REVISION "${LB_GIT_REVISION}")
endif()
if(NOT LB_REVISION MATCHES "^[0-9a-f]+$" AND NOT LB_REVISION STREQUAL "unknown")
  message(FATAL_ERROR "LB_GIT_REVISION must be a hexadecimal commit ID")
endif()
set(LB_BUILD_JSON "{\"version\":\"${PROJECT_VERSION}\",\"revision\":\"${LB_REVISION}\",\"dirty\":${LB_DIRTY_JSON},\"compiler\":\"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\",\"build_type\":\"${CMAKE_BUILD_TYPE}\",\"sanitizer\":\"${LB_SANITIZER}\",\"system\":\"${CMAKE_SYSTEM_NAME}\",\"architecture\":\"${CMAKE_SYSTEM_PROCESSOR}\"}")
configure_file("${PROJECT_SOURCE_DIR}/cmake/version.hpp.in"
  "${PROJECT_BINARY_DIR}/generated/lb/version.hpp" @ONLY)
file(WRITE "${PROJECT_BINARY_DIR}/build-info.json" "${LB_BUILD_JSON}\n")
