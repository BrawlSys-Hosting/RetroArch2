include(CMakeSources.cmake)

add_executable(GGPOSyncPerf
	${GGPO_EXAMPLES_PERF_SRC}
)

if(WIN32 AND BUILD_SHARED_LIBS)
	add_definitions(-DGGPO_SHARED_LIB)
endif()

add_common_flags(GGPOSyncPerf)
target_link_libraries(GGPOSyncPerf LINK_PUBLIC GGPO)

if(WIN32)
	target_link_libraries(GGPOSyncPerf LINK_PUBLIC winmm.lib ws2_32.lib)
	target_compile_options(GGPOSyncPerf PRIVATE "/W4" "/WX")
endif()
