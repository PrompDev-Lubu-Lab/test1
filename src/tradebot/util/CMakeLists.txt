add_library(tradebot_util
  sha256.cpp
  zip_reader.cpp)

target_link_libraries(tradebot_util
  PUBLIC tradebot_core
  PRIVATE OpenSSL::Crypto ZLIB::ZLIB)
