
#1. 确定CMake的最低版本需求
CMAKE_MINIMUM_REQUIRED(VERSION 3.0.0)

#2. 确定工程名
PROJECT(TestUnits LANGUAGES CXX)
SET(CMAKE_CXX_STANDARD 17)

SET(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/TestUnits)

#7. 添加源码
file(GLOB SRCS *.cpp ./gtest/*.cc)

SET(LIBS
	WtBtCore
    WTSTools
	WTSUtils
    WtShareHelper)
IF (MSVC)
ELSE(GNUCC)
    LIST(APPEND LIBS pthread boost_filesystem)
	IF(WIN32)
		#psapi用于回放性能测试里读取进程内存
		LIST(APPEND LIBS iconv psapi)
	ENDIF()
ENDIF()

LIST(APPEND INCS "./gtest")

INCLUDE_DIRECTORIES(${INCS})
LINK_DIRECTORIES(${LNKS})

ADD_EXECUTABLE(TestUnits ${SRCS})
TARGET_LINK_LIBRARIES(TestUnits ${LIBS})

#回放性能测试用基础数据文件在dist/common下
GET_FILENAME_COMPONENT(WT_REPO_ROOT "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
TARGET_COMPILE_DEFINITIONS(TestUnits PRIVATE WT_REPO_ROOT="${WT_REPO_ROOT}")

IF (MSVC)
ELSE (GNUCC)
	SET_TARGET_PROPERTIES(TestUnits PROPERTIES
        LINK_FLAGS_RELEASE -s)
ENDIF ()

#8. 注册CTest测试
#回放性能用例以DISABLED_开头，直接运行TestUnits不会执行，这里逐个带上参数单独调起
SET(REPLAY_PERF_CASES bars_1x bars_Mx bars_resample bars_warm ticks_raw ticks_cmp ticks_stagger real_data)
SET(PERF_WORK_DIR ${CMAKE_BINARY_DIR}/perf_run)
FILE(MAKE_DIRECTORY ${PERF_WORK_DIR})

ADD_TEST(NAME replayer_fee
	COMMAND TestUnits --gtest_filter=test_replayer_fee.*
	WORKING_DIRECTORY ${PERF_WORK_DIR})
SET(WT_TESTS replayer_fee)

FOREACH(PERF_CASE ${REPLAY_PERF_CASES})
	ADD_TEST(NAME perf_replay_${PERF_CASE}
		COMMAND TestUnits --gtest_also_run_disabled_tests --gtest_filter=DISABLED_ReplayPerf.${PERF_CASE}
		WORKING_DIRECTORY ${PERF_WORK_DIR})
	SET_TESTS_PROPERTIES(perf_replay_${PERF_CASE} PROPERTIES
		LABELS "perf;replay"
		RUN_SERIAL TRUE
		TIMEOUT 600)
	LIST(APPEND WT_TESTS perf_replay_${PERF_CASE})
ENDFOREACH()

#没有配置真实数据时real_data什么都不测，标记成跳过，免得显示成通过
SET_TESTS_PROPERTIES(perf_replay_real_data PROPERTIES
	SKIP_REGULAR_EXPRESSION "WT_PERF_REAL_PATH not set, skipped")

#libWtShareHelper.dll在上一级bin目录，MinGW运行库在编译器目录，都要加到PATH里，否则exe起不来
SET(TEST_PATH_MODS "PATH=path_list_prepend:$<TARGET_FILE_DIR:WtShareHelper>")
IF (NOT MSVC AND WIN32)
	GET_FILENAME_COMPONENT(MINGW_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
	LIST(APPEND TEST_PATH_MODS "PATH=path_list_prepend:${MINGW_BIN_DIR}")
ENDIF()

SET_TESTS_PROPERTIES(${WT_TESTS} PROPERTIES
	ENVIRONMENT "WT_NO_PAUSE=1"
	ENVIRONMENT_MODIFICATION "${TEST_PATH_MODS}")