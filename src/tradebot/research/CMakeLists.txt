add_library(tradebot_research
  drift.cpp
  research.cpp
  validation.cpp)

target_link_libraries(tradebot_research
  PUBLIC tradebot_core tradebot_backtest tradebot_analytics)
