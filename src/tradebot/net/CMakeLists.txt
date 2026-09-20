add_library(tradebot_net
  http.cpp
  tcp.cpp
  tls.cpp
  url.cpp
  websocket.cpp)

target_link_libraries(tradebot_net
  PUBLIC tradebot_core
  PRIVATE OpenSSL::SSL OpenSSL::Crypto)
