include("${CMAKE_CURRENT_LIST_DIR}/bacnet_sources.cmake")
idf_component_register(SRCS ${BACNET_SOURCES} INCLUDE_DIRS "include" "${BACNET_SRC}" REQUIRES lwip)
target_compile_definitions(${COMPONENT_LIB} PUBLIC ${BACNET_DEFINITIONS})
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format -Wno-unused-function)
