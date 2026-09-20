add_executable(tradebot-collect collect.cpp)
target_link_libraries(tradebot-collect PRIVATE tradebot_options tradebot_market_data tradebot_net)

add_executable(tradebot-fetch fetch.cpp)
target_link_libraries(tradebot-fetch PRIVATE tradebot_options tradebot_market_data tradebot_net)

add_executable(tradebot-ingest ingest.cpp)
target_link_libraries(tradebot-ingest PRIVATE tradebot_options tradebot_storage tradebot_market_data)

add_executable(tradebot-backtest backtest.cpp)
target_link_libraries(tradebot-backtest PRIVATE tradebot_options tradebot_backtest tradebot_strategies)
