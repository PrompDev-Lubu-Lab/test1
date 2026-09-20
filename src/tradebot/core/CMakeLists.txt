add_library(tradebot_core
  config.cpp
  log.cpp
  time.cpp)

target_include_directories(tradebot_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(tradebot_core PUBLIC tradebot_options tradebot_third_party)
