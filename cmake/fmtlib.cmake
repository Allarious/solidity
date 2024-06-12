include(${CMAKE_SOURCE_DIR}/cmake/submodules.cmake)
set(FMT_SYSTEM_HEADERS ON)
initialize_submodule(fmtlib)
add_subdirectory(
	${CMAKE_SOURCE_DIR}/deps/fmtlib
	EXCLUDE_FROM_ALL
)
