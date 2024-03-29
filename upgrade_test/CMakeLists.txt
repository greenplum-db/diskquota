include(${CMAKE_SOURCE_DIR}/cmake/Regress.cmake)

if(NOT DEFINED DISKQUOTA_DDL_CHANGE_CHECK)
  set(DISKQUOTA_DDL_CHANGE_CHECK ON CACHE
    STRING "Skip the DDL updates check. Should not be disabled on CI" FORCE)
endif()

if (${GP_MAJOR_VERSION} EQUAL 6)
	list(APPEND schedule_files
		${CMAKE_CURRENT_SOURCE_DIR}/schedule_1.0--2.0
		${CMAKE_CURRENT_SOURCE_DIR}/schedule_2.0--1.0
		${CMAKE_CURRENT_SOURCE_DIR}/schedule_2.0--2.1
		${CMAKE_CURRENT_SOURCE_DIR}/schedule_2.1--2.0
		${CMAKE_CURRENT_SOURCE_DIR}/schedule_2.1--2.2
		${CMAKE_CURRENT_SOURCE_DIR}/schedule_2.2--2.1
		)
  set(EXPECTED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/expected")
else()
  set(EXPECTED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/expected7")
endif()
list(APPEND schedule_files
  ${CMAKE_CURRENT_SOURCE_DIR}/schedule_2.2--2.3
  ${CMAKE_CURRENT_SOURCE_DIR}/schedule_2.3--2.2
)
regresstarget_add(
  upgradecheck
  INIT_FILE
  ${CMAKE_CURRENT_SOURCE_DIR}/init_file
  SQL_DIR
  ${CMAKE_CURRENT_SOURCE_DIR}/sql
  EXPECTED_DIR
  ${EXPECTED_DIR}
  RESULTS_DIR
  ${CMAKE_CURRENT_SOURCE_DIR}/results
  SCHEDULE_FILE
  ${schedule_files}
  REGRESS_OPTS
  --dbname=contrib_regression)

execute_process(
  COMMAND git describe --tags --abbrev=0
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE latest_tag
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# check whether DDL file (*.sql) is modified
file(GLOB ddl_files ${DISKQUOTA_DDL_DIR}/*.sql)
foreach(ddl IN LISTS ddl_files)
  cmake_path(GET ddl FILENAME ddl)
  execute_process(
    COMMAND
    git diff ${latest_tag} --exit-code ${ddl}
    OUTPUT_QUIET
    WORKING_DIRECTORY ${DISKQUOTA_DDL_DIR}
    RESULT_VARIABLE "${ddl}_modified"
  )

  if("${${ddl}_modified}")
    message(
      NOTICE
      "compared to ${latest_tag}, the DDL file ${ddl} is modified, checking if upgrade test is needed."
    )
    set(DISKQUOTA_DDL_MODIFIED TRUE)
  endif()
endforeach()

# if DDL file modified, insure the last release file passed in
if(DISKQUOTA_DDL_CHANGE_CHECK AND DISKQUOTA_DDL_MODIFIED AND NOT DEFINED DISKQUOTA_LAST_RELEASE_PATH)
  message(
    FATAL_ERROR
      "DDL file modify detected, upgrade test is required. Add -DDISKQUOTA_LAST_RELEASE_PATH=/<path>/diskquota-<version>-<os>_<arch>.tar.gz. And re-try the generation"
  )
endif()

# check if current version is compatible with the upgrade strategy
if(DISKQUOTA_DDL_MODIFIED AND DEFINED DISKQUOTA_LAST_RELEASE_PATH)
  message(NOTICE "current version ${DISKQUOTA_VERSION}")
  message(NOTICE "last    version ${DISKQUOTA_LAST_VERSION}")

  # if 1.0.a = 1.0.b reject
  if("${DISKQUOTA_MAJOR_VERSION}.${DISKQUOTA_MINOR_VERSION}" STREQUAL
     "${DISKQUOTA_LAST_MAJOR_VERSION}.${DISKQUOTA_LAST_MINOR_VERSION}")
    message(FATAL_ERROR "should bump at last one minor version")
  endif()

  # if 1.0.a to 1.2.b reject
  math(EXPR DISKQUOTA_NEXT_MINOR_VERSION "${DISKQUOTA_LAST_MINOR_VERSION} + 1")
  if(("${DISKQUOTA_MAJOR_VERSION}" STREQUAL "${DISKQUOTA_LAST_MAJOR_VERSION}")
     AND (NOT "${DISKQUOTA_MINOR_VERSION}" STREQUAL
          "${DISKQUOTA_NEXT_MINOR_VERSION}"))
    message(FATAL_ERROR "should not skip any minor version")
  endif()

  # if 1.a.a to 3.a.a reject
  math(EXPR DISKQUOTA_NEXT_MAJOR_VERSION "${DISKQUOTA_LAST_MAJOR_VERSION} + 1")
  if((NOT "${DISKQUOTA_MAJOR_VERSION}" STREQUAL
      "${DISKQUOTA_LAST_MAJOR_VERSION}")
     AND (NOT "${DISKQUOTA_NEXT_MAJOR_VERSION}" STREQUAL
          "${DISKQUOTA_MAJOR_VERSION}"))
    message(FATAL_ERROR "should not skip any major version")
  endif()

  message(
    NOTICE
    "upgrade from ${DISKQUOTA_LAST_VERSION} to ${DISKQUOTA_VERSION} is available"
  )
endif()

# upgrade test is not needed in feature development
