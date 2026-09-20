add_library(tradebot_live
  feed_health.cpp
  journal.cpp
  live_scheduler.cpp
  metrics.cpp
  runtime.cpp)

target_link_libraries(tradebot_live
  PUBLIC tradebot_core tradebot_market_data tradebot_execution tradebot_portfolio tradebot_risk
         tradebot_strategy tradebot_backtest tradebot_analytics tradebot_net tradebot_gateway)
