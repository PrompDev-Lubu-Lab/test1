# Single test runner executable; each module adds its sources here.
add_executable(tradebot_tests
  main.cpp)
target_link_libraries(tradebot_tests PRIVATE doctest tradebot_options tradebot_core tradebot_net
  tradebot_market_data
  OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)

add_subdirectory(core)
add_subdirectory(net)
add_subdirectory(market_data)

add_test(NAME tradebot_tests COMMAND tradebot_tests)
