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

add_executable(tradebot-live live.cpp)
target_link_libraries(tradebot-live PRIVATE tradebot_options tradebot_live tradebot_strategies)

# tradebot-paper is tradebot-live pinned to paper mode: it cannot send an order.
add_executable(tradebot-paper live.cpp)
target_compile_definitions(tradebot-paper PRIVATE TRADEBOT_PAPER_ONLY=1)
target_link_libraries(tradebot-paper PRIVATE tradebot_options tradebot_live tradebot_strategies)

add_executable(tradebot-bench bench.cpp)
target_link_libraries(tradebot-bench PRIVATE tradebot_options tradebot_backtest tradebot_strategies tradebot_replay tradebot_storage)

add_executable(tradebot-healthcheck healthcheck.cpp)
target_link_libraries(tradebot-healthcheck PRIVATE tradebot_options tradebot_live)

add_executable(tradebot-monitor monitor.cpp)
target_link_libraries(tradebot-monitor PRIVATE tradebot_options tradebot_research tradebot_live tradebot_strategies)
