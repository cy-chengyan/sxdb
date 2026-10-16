/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "datasets.h"

#include "common/compiler.h"
#include "common/rng.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define NANOS_PER_SECOND INT64_C(1000000000)
#define NANOS_PER_MILLI  INT64_C(1000000)
#define NANOS_PER_MICRO  INT64_C(1000)

/* 2026-10-16T00:00:00Z in nanoseconds. */
#define START_TIME_NS (INT64_C(1792108800) * NANOS_PER_SECOND)

#define TWO_PI 6.283185307179586

/* Exponentially distributed value with the given mean, for gaps between events. */
static double exponential(sx_rng* rng, double mean) {
    double uniform = sx_rng_next_double(rng);
    return -mean * log(1.0 - uniform);
}

/* Roughly normal value (sum of uniforms), cheap and good enough for noise. */
static double gaussian(sx_rng* rng, double stddev) {
    double sum = 0.0;
    for (int i = 0; i < 4; i++) {
        sum += sx_rng_next_double(rng) - 0.5;
    }
    return sum * stddev * 1.7320508;
}

/* ---------------------------------------------------------------------------------------- */
/* 64-bit integers                                                                          */
/* ---------------------------------------------------------------------------------------- */

static bool gen_ts_regular(bench_column* column, uint64_t seed) {
    SX_UNUSED(seed);
    for (uint32_t i = 0; i < column->count; i++) {
        column->ints[i] = START_TIME_NS + (int64_t)i * NANOS_PER_SECOND;
    }
    return true;
}

static bool gen_ts_ticks(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    int64_t now = START_TIME_NS;
    for (uint32_t i = 0; i < column->count; i++) {
        now += (int64_t)exponential(&rng, 300.0) * NANOS_PER_MILLI;
        column->ints[i] = now;
    }
    return true;
}

static bool gen_ts_jitter(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    for (uint32_t i = 0; i < column->count; i++) {
        int64_t jitter_us = (int64_t)sx_rng_below(&rng, 101) - 50;
        column->ints[i] =
            START_TIME_NS + (int64_t)i * 10 * NANOS_PER_MILLI + jitter_us * NANOS_PER_MICRO;
    }
    return true;
}

static bool gen_counter(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    int64_t total = 1000000000;
    for (uint32_t i = 0; i < column->count; i++) {
        total += (int64_t)sx_rng_below(&rng, 1000);
        column->ints[i] = total;
    }
    return true;
}

static bool gen_volume_lots(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    for (uint32_t i = 0; i < column->count; i++) {
        int64_t lots = 1 + (int64_t)exponential(&rng, 20.0);
        if (lots > 1000) {
            lots = 1000;
        }
        column->ints[i] = lots * 100;
    }
    return true;
}

static bool gen_decimal_price(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    /* DECIMAL(18,4) with a 0.01 tick: 1856.00 is stored as 18560000. */
    int64_t ticks = 185600;
    for (uint32_t i = 0; i < column->count; i++) {
        ticks += (int64_t)sx_rng_below(&rng, 11) - 5;
        column->ints[i] = ticks * 100;
    }
    return true;
}

static bool gen_series_ids(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    /* Sparse series packed into shared blocks: ids ascend, each repeated a few times. */
    int64_t id = 100000;
    uint32_t remaining = 0;
    for (uint32_t i = 0; i < column->count; i++) {
        if (remaining == 0) {
            id += 1 + (int64_t)sx_rng_below(&rng, 3);
            remaining = 5 + (uint32_t)sx_rng_below(&rng, 30);
        }
        column->ints[i] = id;
        remaining--;
    }
    return true;
}

static bool gen_random_int(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    for (uint32_t i = 0; i < column->count; i++) {
        column->ints[i] = (int64_t)sx_rng_next(&rng);
    }
    return true;
}

/* ---------------------------------------------------------------------------------------- */
/* Doubles                                                                                  */
/* ---------------------------------------------------------------------------------------- */

static bool gen_price_2dp(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    int64_t cents = 185600;
    for (uint32_t i = 0; i < column->count; i++) {
        cents += (int64_t)sx_rng_below(&rng, 11) - 5;
        column->doubles[i] = (double)cents / 100.0;
    }
    return true;
}

static bool gen_cpu_percent(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    double level = 35.0;
    for (uint32_t i = 0; i < column->count; i++) {
        level += gaussian(&rng, 2.0);
        if (level < 0.0) {
            level = 0.0;
        }
        if (level > 100.0) {
            level = 100.0;
        }
        column->doubles[i] = round(level * 10.0) / 10.0;
    }
    return true;
}

static bool gen_temperature(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    for (uint32_t i = 0; i < column->count; i++) {
        double daily = 22.0 + 6.0 * sin((double)i * TWO_PI / 86400.0);
        column->doubles[i] = round((daily + gaussian(&rng, 0.2)) * 100.0) / 100.0;
    }
    return true;
}

static bool gen_float_sensor(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    float value = 3.3f;
    for (uint32_t i = 0; i < column->count; i++) {
        value += (float)gaussian(&rng, 0.001);
        column->doubles[i] = (double)value;
    }
    return true;
}

static bool gen_moving_average(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    /* A 7-sample moving average of a 2-decimal price: full-precision results. */
    double window[7] = {0};
    double sum = 0.0;
    int64_t cents = 185600;
    for (uint32_t i = 0; i < column->count; i++) {
        cents += (int64_t)sx_rng_below(&rng, 11) - 5;
        double price = (double)cents / 100.0;
        sum += price - window[i % 7];
        window[i % 7] = price;
        column->doubles[i] = sum / 7.0;
    }
    return true;
}

static bool gen_random_double(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    for (uint32_t i = 0; i < column->count; i++) {
        column->doubles[i] = sx_rng_next_double(&rng);
    }
    return true;
}

/* ---------------------------------------------------------------------------------------- */
/* Booleans and strings                                                                     */
/* ---------------------------------------------------------------------------------------- */

static bool gen_rare_flags(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    for (uint32_t i = 0; i < column->count; i++) {
        column->bools[i] = sx_rng_below(&rng, 100) == 0 ? 1 : 0;
    }
    return true;
}

static bool gen_trade_side(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    for (uint32_t i = 0; i < column->count; i++) {
        const char* side = sx_rng_below(&rng, 2) == 0 ? "B" : "S";
        if (!bench_column_set_string(column, i, side, 1)) {
            return false;
        }
    }
    return true;
}

static bool gen_instrument_codes(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    for (uint32_t i = 0; i < column->count; i++) {
        char code[16];
        int length = snprintf(code, sizeof(code), "600%03u.SH", (unsigned)sx_rng_below(&rng, 300));
        if (!bench_column_set_string(column, i, code, (size_t)length)) {
            return false;
        }
    }
    return true;
}

static bool gen_order_ids(bench_column* column, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    uint64_t id = 20261016000000ull;
    for (uint32_t i = 0; i < column->count; i++) {
        char text[32];
        id += 1 + sx_rng_below(&rng, 5);
        int length = snprintf(text, sizeof(text), "ORD-%llu", (unsigned long long)id);
        if (!bench_column_set_string(column, i, text, (size_t)length)) {
            return false;
        }
    }
    return true;
}

const bench_dataset bench_datasets[] = {
    {"ts_regular_1s", "timestamps every second (ns)", BENCH_TYPE_INT64, gen_ts_regular},
    {"ts_ticks_ms", "trade timestamps, random ms gaps (ns)", BENCH_TYPE_INT64, gen_ts_ticks},
    {"ts_jitter_us", "10 ms sampling, +-50 us jitter (ns)", BENCH_TYPE_INT64, gen_ts_jitter},
    {"counter", "monotonic counter, random increments", BENCH_TYPE_INT64, gen_counter},
    {"volume_lots", "trade volume in lots of 100", BENCH_TYPE_INT64, gen_volume_lots},
    {"decimal_price", "DECIMAL(18,4) price, 0.01 ticks", BENCH_TYPE_INT64, gen_decimal_price},
    {"series_ids", "ascending series ids in runs", BENCH_TYPE_INT64, gen_series_ids},
    {"random_int64", "uniform random (worst case)", BENCH_TYPE_INT64, gen_random_int},
    {"price_2dp", "stock price walk, 2 decimals", BENCH_TYPE_DOUBLE, gen_price_2dp},
    {"cpu_percent_1dp", "CPU usage 0-100, 1 decimal", BENCH_TYPE_DOUBLE, gen_cpu_percent},
    {"temperature_2dp", "sensor temperature, 2 decimals", BENCH_TYPE_DOUBLE, gen_temperature},
    {"float32_sensor", "float32 readings widened", BENCH_TYPE_DOUBLE, gen_float_sensor},
    {"moving_average", "full-precision computed values", BENCH_TYPE_DOUBLE, gen_moving_average},
    {"random_double", "uniform random in [0,1) (worst case)", BENCH_TYPE_DOUBLE, gen_random_double},
    {"rare_flags", "boolean, 1% true", BENCH_TYPE_BOOL, gen_rare_flags},
    {"trade_side", "\"B\" or \"S\"", BENCH_TYPE_STRING, gen_trade_side},
    {"instrument_codes", "300 distinct tickers", BENCH_TYPE_STRING, gen_instrument_codes},
    {"order_ids", "unique order ids", BENCH_TYPE_STRING, gen_order_ids},
};

const size_t bench_dataset_count = sizeof(bench_datasets) / sizeof(bench_datasets[0]);
