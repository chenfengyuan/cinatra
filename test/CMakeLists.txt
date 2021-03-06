CMAKE_MINIMUM_REQUIRED(VERSION 2.8.12)
PROJECT(cinatra_test)
SET(CMAKE_VERBOSE_MAKEFILE ON)
SET(CMAKE_CXX_COMPILER clang++)
ADD_COMPILE_OPTIONS("-std=c++1y")
ADD_COMPILE_OPTIONS("-Wall")
ADD_COMPILE_OPTIONS("-Wextra")
SET(TEST_SOURCES
	test.cpp
	request_parser/request_parser.cpp
	utils/case_map.cpp
	utils/ncase_multi_map.cpp
	response/response.cpp
	router/router.cpp
)
ADD_EXECUTABLE(cinatra_test ${TEST_SOURCES})
INCLUDE_DIRECTORIES(../include)
IF(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	TARGET_LINK_LIBRARIES(cinatra_test boost_system-mt pthread boost_coroutine-mt boost_thread-mt)
ELSE(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	TARGET_LINK_LIBRARIES(cinatra_test boost_system pthread boost_coroutine boost_thread)
ENDIF(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")

ADD_CUSTOM_TARGET(run_test COMMAND ${CMAKE_BINARY_DIR}/cinatra_test DEPENDS cinatra_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
ADD_CUSTOM_TARGET(debug
	COMMAND ${CMAKE_COMMAND} -DCMAKE_BUILD_TYPE=Debug ${CMAKE_SOURCE_DIR}
	COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target all
	COMMENT "Switch CMAKE_BUILD_TYPE to Debug"

)
ADD_CUSTOM_TARGET(release
	COMMAND ${CMAKE_COMMAND} -DCMAKE_BUILD_TYPE=Release ${CMAKE_SOURCE_DIR}
	COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target all
	COMMENT "Switch CMAKE_BUILD_TYPE to Release"
)
