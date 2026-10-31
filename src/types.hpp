#pragma once
#include <cstdint>

enum class OrderType : uint8_t { Limit = 0, Market = 1, Ioc = 2, Fok = 3 };

struct Order {
    OrderType type;
    uint32_t  qty;
    uint32_t  lot_size;
    int64_t   price_ticks;
    double    tick_size;
    uint32_t  instrument;
};

struct Result {
    uint32_t lots;
    uint32_t odd_qty;
    int64_t  notional_milli;
    int64_t  fee_micro;
};
