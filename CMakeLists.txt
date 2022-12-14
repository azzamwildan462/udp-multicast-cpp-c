CMAKE_MINIMUM_REQUIRED(VERSION 3.0)
PROJECT( IRISCOMM )

set( CMAKE_MODULE_PATH ${CMAKE_MODULES_PATH} ${CMAKE_SOURCE_DIR}/CMakeModules )
set( LMDB_LIBRARY_PATH ${CMAKE_SOURCE_DIR}/dependencies/lmdb/libraries/liblmdb/liblmdb.so)
set( LMDB_INCLUDE_PATH ${CMAKE_SOURCE_DIR}/dependencies/lmdb/libraries/liblmdb/)

find_package(LZ4 REQUIRED)
if(LZ4_FOUND)
    include_directories(SYSTEM ${LZ4_INCLUDE_DIR})
endif()

# find_package(yaml-cpp REQUIRED)
# find_package(Boost COMPONENTS system filesystem REQUIRED)

add_library(LMDB_LIB SHARED IMPORTED)
set_target_properties(LMDB_LIB PROPERTIES
IMPORTED_LOCATION ${LMDB_LIBRARY_PATH}
INTERFACE_INCLUDE_DIRECTORIES ${LMDB_INCLUDE_PATH}
)

add_executable(rt_comm src/rt_comm.cpp)
target_link_libraries(rt_comm ${LZ4_LIBRARIES} z -pthread)
set_target_properties(rt_comm PROPERTIES COMPILE_FLAGS "-fPIC")

add_executable(comm_adapter_bs src/comm_adapter_bs.cpp)
target_link_libraries(comm_adapter_bs ${LZ4_LIBRARIES} z -pthread LMDB_LIB)
set_target_properties(comm_adapter_bs PROPERTIES COMPILE_FLAGS "-fPIC")

# Pre-served to be used in ROS environtments
# add_executable(comm_multicast src/comm_multicast.cpp)
# target_link_libraries(comm_multicast LMDB_LIB ${LZ4_LIBRARIES} z)





