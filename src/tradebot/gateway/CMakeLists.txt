add_library(tradebot_gateway
  binance_gateway.cpp
  binance_rest.cpp
  signing.cpp
  user_stream.cpp)

target_link_libraries(tradebot_gateway
  PUBLIC tradebot_core tradebot_net tradebot_execution tradebot_portfolio
  PRIVATE OpenSSL::Crypto)
