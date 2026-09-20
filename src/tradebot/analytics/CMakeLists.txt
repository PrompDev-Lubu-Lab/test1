add_library(tradebot_analytics
  analytics.cpp)

target_link_libraries(tradebot_analytics
  PUBLIC tradebot_core tradebot_backtest tradebot_portfolio)
