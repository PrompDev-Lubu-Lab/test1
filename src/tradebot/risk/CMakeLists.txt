add_library(tradebot_risk
  risk_manager.cpp)

target_link_libraries(tradebot_risk
  PUBLIC tradebot_core tradebot_execution tradebot_portfolio)
