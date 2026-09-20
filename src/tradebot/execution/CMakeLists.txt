add_library(tradebot_execution
  simulated_exchange.cpp)

target_link_libraries(tradebot_execution
  PUBLIC tradebot_core tradebot_market_data tradebot_replay)
