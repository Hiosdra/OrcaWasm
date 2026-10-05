# FindOpenSSL.cmake — WASM stub.
# OrcaSlicer uses OpenSSL::Crypto only for MD5 checksums (config files and the
# Metadata/plate_N.gcode.md5 entries of sliced 3MF projects). We provide a
# header-only MD5 implementation + empty targets, so no OpenSSL is linked.
set(_SSL_STUB_DIR "${CMAKE_CURRENT_LIST_DIR}/../wasm/shims-common")

if(NOT TARGET OpenSSL::Crypto)
  add_library(OpenSSL::Crypto INTERFACE IMPORTED GLOBAL)
  target_include_directories(OpenSSL::Crypto INTERFACE "${_SSL_STUB_DIR}")
endif()
if(NOT TARGET OpenSSL::SSL)
  add_library(OpenSSL::SSL INTERFACE IMPORTED GLOBAL)
  target_include_directories(OpenSSL::SSL INTERFACE "${_SSL_STUB_DIR}")
endif()

set(OPENSSL_FOUND TRUE)
set(OPENSSL_INCLUDE_DIR "${_SSL_STUB_DIR}")
set(OPENSSL_LIBRARIES OpenSSL::Crypto)
set(OPENSSL_CRYPTO_LIBRARY OpenSSL::Crypto)
set(OPENSSL_SSL_LIBRARY OpenSSL::SSL)
set(OpenSSL_FOUND TRUE)
