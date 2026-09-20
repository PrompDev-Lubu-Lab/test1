add_library(tradebot_replay
  event_source.cpp
  replay_engine.cpp)

target_link_libraries(tradebot_replay
  PUBLIC tradebot_core tradebot_market_data tradebot_storage)
