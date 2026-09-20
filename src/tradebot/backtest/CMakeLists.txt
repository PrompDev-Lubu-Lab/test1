add_library(tradebot_backtest
  backtest.cpp)

target_link_libraries(tradebot_backtest
  PUBLIC tradebot_core tradebot_storage tradebot_replay tradebot_execution tradebot_portfolio
         tradebot_risk tradebot_strategy tradebot_util)
