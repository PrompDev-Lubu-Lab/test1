add_library(tradebot_market_data
  raw_capture.cpp)

target_link_libraries(tradebot_market_data
  PUBLIC tradebot_core
  PRIVATE ZLIB::ZLIB)
