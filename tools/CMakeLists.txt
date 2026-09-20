add_executable(tradebot-collect collect.cpp)
target_link_libraries(tradebot-collect PRIVATE tradebot_options tradebot_market_data tradebot_net)

add_executable(tradebot-fetch fetch.cpp)
target_link_libraries(tradebot-fetch PRIVATE tradebot_options tradebot_market_data tradebot_net)
