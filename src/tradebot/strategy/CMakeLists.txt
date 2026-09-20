add_library(tradebot_strategy
  runner.cpp)

target_link_libraries(tradebot_strategy
  PUBLIC tradebot_core tradebot_market_data tradebot_execution tradebot_portfolio tradebot_replay)
