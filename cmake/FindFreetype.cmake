# FindFreetype.cmake — WASM stub.
# Font rendering is not needed for headless G-code slicing.
set(_FT_STUB_DIR "${CMAKE_CURRENT_LIST_DIR}/../wasm/shims-common")

if(NOT TARGET Freetype::Freetype)
  add_library(Freetype::Freetype INTERFACE IMPORTED GLOBAL)
  target_include_directories(Freetype::Freetype INTERFACE "${_FT_STUB_DIR}")
endif()
set(FREETYPE_FOUND TRUE)
set(Freetype_FOUND TRUE)
set(FREETYPE_INCLUDE_DIRS "${_FT_STUB_DIR}")
set(FREETYPE_LIBRARIES Freetype::Freetype)
