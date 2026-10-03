cmake_minimum_required(VERSION 3.18)

project(skybench LANGUAGES C CXX)

if(NOT DEFINED SKSE_SRC)
	set(SKSE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/skse-2.3.1/skse64_2_03_01/src/x/skse64-2.3.1")
endif()
get_filename_component(SKSE_SRC "${SKSE_SRC}" ABSOLUTE)

if(NOT EXISTS "${SKSE_SRC}/skse64/PluginAPI.h")
	message(FATAL_ERROR "skse64 source tree not found at ${SKSE_SRC} (set -DSKSE_SRC=...)")
endif()

add_library(skybench SHARED
	src/skybench.cpp
	src/address_library.cpp
	src/cascade_cull.cpp
	src/cascade_census.cpp
	src/cascade_halfrate.cpp
	src/call_patch.cpp
	src/point_shadow_cull.cpp
	src/occlusion_probe.cpp
	src/precip_gate.cpp
	src/havok_step.cpp
	src/water_halfrate.cpp
	src/frame_timer.cpp
	src/allocator.cpp
	src/allocator_patch.cpp
	src/allocator_diag.cpp
	src/halfrate_switch.cpp
	"${SKSE_SRC}/skse64_common/Relocation.cpp"
	"${SKSE_SRC}/skse64/GameTypes.cpp"
	"${SKSE_SRC}/skse64/GameEvents.cpp"
	"${SKSE_SRC}/skse64/GameAPI.cpp"
)

target_compile_features(skybench PRIVATE cxx_std_17)

target_include_directories(skybench PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${SKSE_SRC}"
)

# every skse64 source file assumes it's been force-included with
# common/IPrefix.h (see skse64/sheets/ForcedInclude.props); we don't have
# that external "common" package, so we force our own compat shim in its
# place for every translation unit, including the vendored skse64 sources.
target_compile_options(skybench PRIVATE
	/FI${CMAKE_CURRENT_SOURCE_DIR}/include/skse_compat.h
)

target_compile_definitions(skybench PRIVATE
	RUNTIME
	RUNTIME_VERSION=0x01070680	# RUNTIME_VERSION_1_7_104, packed (see skse64_common/skse_version.h)
)

set_target_properties(skybench PROPERTIES
	OUTPUT_NAME "skybench"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/skybench-plugin"
	MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

if(MSVC)
	target_compile_options(skybench PRIVATE /W3 /permissive-)
	target_link_options(skybench PRIVATE /DEBUG)
endif()

# mimalloc (extern/mimalloc, mit, see VENDOR.txt) as one static C unit for
# src/allocator.cpp. its own target, so skse_compat.h is not forced into it.
add_library(skybench_mimalloc STATIC extern/mimalloc/src/static.c)
target_include_directories(skybench_mimalloc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/extern/mimalloc/include")
target_compile_definitions(skybench_mimalloc PRIVATE MI_DEBUG=0)
set_target_properties(skybench_mimalloc PROPERTIES
	MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_link_libraries(skybench PRIVATE skybench_mimalloc)

# offline checks for the allocator (no game needed). off by default.
option(SKYBENCH_BUILD_TESTS "build skybench_alloc_test" OFF)
if(SKYBENCH_BUILD_TESTS)
	add_executable(skybench_alloc_test test/allocator_test.cpp src/allocator_patch.cpp)
	target_compile_features(skybench_alloc_test PRIVATE cxx_std_17)
	target_link_libraries(skybench_alloc_test PRIVATE skybench_mimalloc)
	set_target_properties(skybench_alloc_test PROPERTIES
		MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
	)
	if(MSVC)
		target_compile_options(skybench_alloc_test PRIVATE /W3 /permissive-)
	endif()
endif()
