set(SAMPLE sample.hello_world)
include(${CMAKE_SOURCE_DIR}/cmake/sample.cmake)
target_link_libraries(${SAMPLE} boost_system-mt boost_thread-mt )
