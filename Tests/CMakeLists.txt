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
FetchContent_Declare(
	googletest
	GIT_REPOSITORY https://github.com/google/googletest.git
	GIT_TAG        v1.15.2
)

if(WIN32)
	set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
endif()

FetchContent_MakeAvailable(googletest)

set(PLUGIN_PRIVATE_DIR
	${CMAKE_CURRENT_SOURCE_DIR}/../Source/Framedash/Private
)
set(PLUGIN_PROTO_DIR ${PLUGIN_PRIVATE_DIR}/Proto)
set(PLUGIN_NANOPB_DIR ${PLUGIN_PRIVATE_DIR}/ThirdParty/nanopb)

enable_testing()
include(GoogleTest)

add_executable(framedash_ue5_tests
	AddressPlannerTests.cpp
	BatchPolicyTests.cpp
	CameraMathTests.cpp
	EndpointSecurityTests.cpp
	FramedashFieldClampsTests.cpp
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

if(MSVC)
	target_compile_options(framedash_ue5_tests PRIVATE
		$<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>
		$<$<COMPILE_LANGUAGE:C>:/utf-8>
	)
else()
	target_compile_options(framedash_ue5_tests PRIVATE
		$<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
	)
endif()

set_source_files_properties(
	${PLUGIN_PROTO_DIR}/telemetry.pb.c
	${PLUGIN_NANOPB_DIR}/pb_common.c
	${PLUGIN_NANOPB_DIR}/pb_decode.c
	${PLUGIN_NANOPB_DIR}/pb_encode.c
	PROPERTIES
		COMPILE_OPTIONS "$<$<C_COMPILER_ID:MSVC>:/w>;$<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>"
)

gtest_discover_tests(framedash_ue5_tests)
