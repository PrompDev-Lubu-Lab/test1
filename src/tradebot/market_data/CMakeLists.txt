add_library(tradebot_market_data
  binance/bulk_fetcher.cpp
  binance/collector.cpp
  raw_capture.cpp)

target_link_libraries(tradebot_market_data
  PUBLIC tradebot_core tradebot_net tradebot_util
  PRIVATE ZLIB::ZLIB)
