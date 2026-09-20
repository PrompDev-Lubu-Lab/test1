add_library(tradebot_storage
  event_file.cpp
  event_store.cpp
  ingest.cpp)

target_link_libraries(tradebot_storage
  PUBLIC tradebot_core tradebot_market_data tradebot_util
  PRIVATE ZLIB::ZLIB)
