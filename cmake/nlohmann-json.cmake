include(${CMAKE_SOURCE_DIR}/cmake/submodules.cmake)
set(JSON_Install OFF CACHE INTERNAL "")
initialize_submodule(nlohmann-json)
add_subdirectory(
	${CMAKE_SOURCE_DIR}/deps/nlohmann-json
	EXCLUDE_FROM_ALL
)

