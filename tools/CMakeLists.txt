add_executable(tradebot-collect collect.cpp)
target_link_libraries(tradebot-collect PRIVATE tradebot_options tradebot_market_data tradebot_net)

add_executable(tradebot-fetch fetch.cpp)
target_link_libraries(tradebot-fetch PRIVATE tradebot_options tradebot_market_data tradebot_net)

add_executable(tradebot-ingest ingest.cpp)
target_link_libraries(tradebot-ingest PRIVATE tradebot_options tradebot_storage tradebot_market_data)

add_executable(tradebot-backtest backtest.cpp)
target_link_libraries(tradebot-backtest PRIVATE tradebot_options tradebot_backtest tradebot_strategies)

add_executable(tradebot-analyze analyze.cpp)
target_link_libraries(tradebot-analyze PRIVATE tradebot_options tradebot_analytics)

add_executable(tradebot-research research.cpp)
target_link_libraries(tradebot-research PRIVATE tradebot_options tradebot_research tradebot_strategies)

add_executable(tradebot-paper paper.cpp)
target_link_libraries(tradebot-paper PRIVATE tradebot_options tradebot_live tradebot_strategies)
