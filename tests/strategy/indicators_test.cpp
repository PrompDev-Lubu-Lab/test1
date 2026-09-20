#include "tradebot/strategy/indicators.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::strategy;
using namespace tradebot::literals;

TEST_CASE("Sma / Ema") {
    Sma sma(3);
    CHECK_FALSE(sma.ready());
    sma.push(1);
    sma.push(2);
    CHECK_FALSE(sma.value().has_value());
    sma.push(3);
    CHECK(*sma.value() == doctest::Approx(2.0));
    sma.push(6);
    CHECK(*sma.value() == doctest::Approx(11.0 / 3.0));

    Ema ema(3);  // alpha = 0.5
    ema.push(10);
    ema.push(20);
    CHECK_FALSE(ema.ready());
    ema.push(30);
    // 10 -> 15 -> 22.5
    CHECK(*ema.value() == doctest::Approx(22.5));
}

TEST_CASE("RollingStats: mean, stddev, zscore") {
    RollingStats st(4);
    for (double v : {2.0, 4.0, 4.0, 4.0}) st.push(v);
    CHECK(*st.mean() == doctest::Approx(3.5));
    CHECK(*st.stddev() == doctest::Approx(std::sqrt(0.75)));
    CHECK(*st.zscore() == doctest::Approx(0.5 / std::sqrt(0.75)));
    st.push(4.0);  // window 4,4,4,4: stddev 0 -> zscore undefined
    CHECK(*st.stddev() == doctest::Approx(0.0));
    CHECK_FALSE(st.zscore().has_value());
    RollingStats empty(2);
    CHECK_FALSE(empty.zscore().has_value());
}

TEST_CASE("RollingExtrema") {
    RollingExtrema ex(3);
    ex.push(5);
    ex.push(1);
    CHECK_FALSE(ex.ready());
    ex.push(3);
    CHECK(*ex.max() == 5);
    CHECK(*ex.min() == 1);
    ex.push(2);  // window 1,3,2
    CHECK(*ex.max() == 3);
    CHECK(*ex.min() == 1);
    ex.push(4);  // 3,2,4
    CHECK(*ex.max() == 4);
    CHECK(*ex.min() == 2);
    ex.push(0);  // 2,4,0
    CHECK(*ex.max() == 4);
    CHECK(*ex.min() == 0);
}

TEST_CASE("Atr and Rsi") {
    auto candle = [](double o, double h, double l, double c) {
        market_data::Candle k;
        k.open = Price::from_double(o);
        k.high = Price::from_double(h);
        k.low = Price::from_double(l);
        k.close = Price::from_double(c);
        return k;
    };
    Atr atr(2);
    atr.push(candle(10, 12, 9, 11));  // TR 3
    CHECK_FALSE(atr.ready());
    atr.push(candle(11, 15, 10, 14));  // TR max(5, |15-11|, |10-11|) = 5 -> seed (3+5)/2 = 4
    CHECK(*atr.value() == doctest::Approx(4.0));
    atr.push(candle(14, 14, 12, 13));  // TR max(2, 0, 2) = 2 -> (4*1 + 2)/2 = 3
    CHECK(*atr.value() == doctest::Approx(3.0));

    Rsi rsi(3);
    for (double c : {10, 11, 12, 13}) rsi.push(c);  // 3 gains
    CHECK(*rsi.value() == doctest::Approx(100.0));
    Rsi rsi2(2);
    for (double c : {10, 12, 11}) rsi2.push(c);  // gains 2, loss 1 -> avg gain 1, avg loss 0.5 -> RS 2 -> 66.67
    CHECK(*rsi2.value() == doctest::Approx(200.0 / 3.0));
    rsi2.push(10);  // loss 1: avg gain (1*1+0)/2 = 0.5, avg loss (0.5+1)/2 = 0.75 -> RS 2/3 -> 40
    CHECK(*rsi2.value() == doctest::Approx(40.0));
}
