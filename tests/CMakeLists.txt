include(${CMAKE_SOURCE_DIR}/cmake/Regress.cmake)

RegressTarget_Add(regress
    INIT_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/init_file
    SQL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/regress/sql
    EXPECTED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/regress/expected
    RESULTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/regress/results
    DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data
    SCHEDULE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/regress/diskquota_schedule
    REGRESS_OPTS
    --load-extension=gp_inject_fault
    --dbname=contrib_regression)

RegressTarget_Add(isolation2
    REGRESS_TYPE
    isolation2
    INIT_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/init_file
    SQL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/isolation2/sql
    EXPECTED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/isolation2/expected
    RESULTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/isolation2/results
    DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data
    SCHEDULE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/isolation2/isolation2_schedule
    REGRESS_OPTS
    --load-extension=gp_inject_fault
    --dbname=isolation2test)

add_custom_target(installcheck)
add_dependencies(installcheck isolation2 regress)

# Example to run test_truncate infinite times
# RegressTarget_Add(regress_config
#     INIT_FILE
#     ${CMAKE_CURRENT_SOURCE_DIR}/init_file
#     SQL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/regress/sql
#     EXPECTED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/regress/expected
#     RESULTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/regress/results
#     DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data
#     REGRESS
#     config test_create_extension
#     REGRESS_OPTS
#     --load-extension=gp_inject_fault
#     --dbname=contrib_regression)
# RegressTarget_Add(regress_truncate_loop
#     INIT_FILE
#     ${CMAKE_CURRENT_SOURCE_DIR}/init_file
#     SQL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/regress/sql
#     EXPECTED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/regress/expected
#     RESULTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/regress/results
#     DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data
#     REGRESS
#     test_truncate
#     RUN_TIMES -1
#     REGRESS_OPTS
#     --load-extension=gp_inject_fault
#     --dbname=contrib_regression
#     --use-existing)
# add_dependencies(regress_truncate_loop regress_config)
# add_dependencies(installcheck regress_truncate_loop)

add_executable(test_unit_config_parse unit_test/test_unit_config_parse.c)
set_target_properties(
  test_unit_config_parse
  PROPERTIES OUTPUT_NAME "test_unit_config_parse"
             PREFIX ""
             RUNTIME_OUTPUT_DIRECTORY "unit_test/"
             C_STANDARD 99
             LINKER_LANGUAGE "C")
target_include_directories(
  test_unit_config_parse PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${PG_INCLUDE_DIR}
  ${PG_INCLUDE_DIR_SERVER}
  ${deps_INSTALL_DIR}/include)

target_link_libraries(
  test_unit_config_parse
  ${deps_INSTALL_DIR}/lib/libcjson.a
  ${deps_INSTALL_DIR}/lib/libcmocka.so)

add_custom_target(ut 
  COMMAND test_unit_config_parse)
