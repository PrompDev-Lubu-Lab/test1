add_library(tradebot_strategies
  baselines.cpp)

target_link_libraries(tradebot_strategies
  PUBLIC tradebot_strategy)
