cmake_minimum_required(VERSION 3.20)
project(framedash_ue5_tests C CXX)

# Engine-independent unit tests for SDK policy classes.
# These tests do NOT require an UnrealEditor or UE5 build; they exercise
# pure C++ classes extracted from the plugin so retry/flush logic can be
# regression-tested without spinning up the full engine.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
# Pinned to the commit v1.15.2 points at, not the tag: a tag can be moved or
# re-pointed upstream, and this CMake code is fetched and executed on the
# self-hosted runner and on the release host (scripts/package-ue5-plugin.ps1
# gates packaging on this harness), so it must resolve to immutable bytes.
FetchContent_Declare(
	googletest
	GIT_REPOSITORY https://github.com/google/googletest.git
	GIT_TAG        b514bdc898e2951020cbdca1304b75f5950d1f59
)

if(WIN32)
	set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
endif()

FetchContent_MakeAvailable(googletest)

set(PLUGIN_PRIVATE_DIR
	${CMAKE_CURRENT_SOURCE_DIR}/../Source/Framedash/Private
)
set(PLUGIN_PROTO_DIR ${PLUGIN_PRIVATE_DIR}/Proto)
set(PLUGIN_NANOPB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../Source/ThirdParty/nanopb)

enable_testing()
include(GoogleTest)

add_executable(framedash_ue5_tests
	AddressPlannerTests.cpp
	BatchPolicyTests.cpp
	CameraMathTests.cpp
	EndpointSecurityTests.cpp
	FramedashFieldClampsTests.cpp
	FramedashIoStatsTests.cpp
	FramedashMapLoadTimerTests.cpp
	FramedashMemorySampleTests.cpp
	FlushPolicyTests.cpp
	ProtobufSerializerTests.cpp
	RawHttpTests.cpp
	RetryPolicyTests.cpp
	UuidTests.cpp
	${PLUGIN_PRIVATE_DIR}/FramedashAddressPlanner.cpp
	${PLUGIN_PRIVATE_DIR}/FramedashBatchPolicy.cpp
	${PLUGIN_PRIVATE_DIR}/FramedashEndpointSecurity.cpp
	${PLUGIN_PRIVATE_DIR}/FramedashRawHttp.cpp
	${PLUGIN_PRIVATE_DIR}/FramedashFlushPolicy.cpp
	${PLUGIN_PRIVATE_DIR}/FramedashProtobufSerializer.cpp
	${PLUGIN_PRIVATE_DIR}/FramedashRetryPolicy.cpp
	${PLUGIN_PRIVATE_DIR}/FramedashUuid.cpp
	${PLUGIN_PROTO_DIR}/telemetry.pb.c
	${PLUGIN_NANOPB_DIR}/pb_common.c
	${PLUGIN_NANOPB_DIR}/pb_decode.c
	${PLUGIN_NANOPB_DIR}/pb_encode.c
)

target_include_directories(framedash_ue5_tests PRIVATE
	${PLUGIN_PRIVATE_DIR}
	${PLUGIN_PROTO_DIR}
	${PLUGIN_NANOPB_DIR}
)
target_link_libraries(framedash_ue5_tests PRIVATE GTest::gtest_main)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
	target_compile_options(framedash_ue5_tests PRIVATE
		$<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /clang:-fprofile-instr-generate /clang:-fcoverage-mapping>
		$<$<COMPILE_LANGUAGE:C>:/utf-8 /clang:-fprofile-instr-generate /clang:-fcoverage-mapping>
	)
	execute_process(
		COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
		RESULT_VARIABLE CLANG_RESOURCE_DIR_RESULT
		OUTPUT_VARIABLE CLANG_RESOURCE_DIR
		ERROR_VARIABLE CLANG_RESOURCE_DIR_ERROR
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	if(NOT CLANG_RESOURCE_DIR_RESULT EQUAL 0 OR CLANG_RESOURCE_DIR STREQUAL "")
		message(FATAL_ERROR "clang-cl could not resolve its resource directory: ${CLANG_RESOURCE_DIR_ERROR}")
	endif()
	set(CLANG_PROFILE_RUNTIME "${CLANG_RESOURCE_DIR}/lib/windows/clang_rt.profile-x86_64.lib")
	if(NOT EXISTS "${CLANG_PROFILE_RUNTIME}")
		message(FATAL_ERROR "Clang x64 profile runtime not found at ${CLANG_PROFILE_RUNTIME}")
	endif()
	target_link_libraries(framedash_ue5_tests PRIVATE "${CLANG_PROFILE_RUNTIME}")
elseif(MSVC)
	target_compile_options(framedash_ue5_tests PRIVATE
		$<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>
		$<$<COMPILE_LANGUAGE:C>:/utf-8>
	)
else()
	target_compile_options(framedash_ue5_tests PRIVATE
		$<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
	)
endif()

if(MSVC OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_SIMULATE_ID STREQUAL "MSVC"))
	set(THIRD_PARTY_WARNING_OPTION /w)
else()
	set(THIRD_PARTY_WARNING_OPTION -w)
endif()

set_source_files_properties(
	${PLUGIN_PROTO_DIR}/telemetry.pb.c
	${PLUGIN_NANOPB_DIR}/pb_common.c
	${PLUGIN_NANOPB_DIR}/pb_decode.c
	${PLUGIN_NANOPB_DIR}/pb_encode.c
	PROPERTIES COMPILE_OPTIONS "${THIRD_PARTY_WARNING_OPTION}"
)

gtest_discover_tests(framedash_ue5_tests)
