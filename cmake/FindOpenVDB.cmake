# FindOpenVDB.cmake — stub for WASM builds.
if(NOT TARGET OpenVDB::openvdb)
  add_library(OpenVDB::openvdb INTERFACE IMPORTED GLOBAL)
  set_target_properties(OpenVDB::openvdb PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../wasm/shims-common")
endif()
set(OpenVDB_FOUND   TRUE)
set(OPENVDB_FOUND   TRUE)
