add_library(tradebot_portfolio
  portfolio.cpp)

target_link_libraries(tradebot_portfolio
  PUBLIC tradebot_core tradebot_execution tradebot_replay)
