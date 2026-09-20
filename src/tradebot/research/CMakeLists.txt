add_library(tradebot_research
  research.cpp
  validation.cpp)

target_link_libraries(tradebot_research
  PUBLIC tradebot_core tradebot_backtest tradebot_analytics)
