# Single test runner executable; each module adds its sources here.
add_executable(tradebot_tests
  main.cpp)
target_include_directories(tradebot_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(tradebot_tests PRIVATE
  TRADEBOT_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(tradebot_tests PRIVATE doctest tradebot_options tradebot_core tradebot_net
  tradebot_market_data tradebot_util tradebot_storage tradebot_replay tradebot_execution
  tradebot_portfolio tradebot_risk tradebot_strategy
  OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)

add_subdirectory(core)
add_subdirectory(util)
add_subdirectory(net)
add_subdirectory(market_data)
add_subdirectory(storage)
add_subdirectory(replay)
add_subdirectory(execution)
add_subdirectory(portfolio)
add_subdirectory(risk)
add_subdirectory(strategy)

add_test(NAME tradebot_tests COMMAND tradebot_tests)
