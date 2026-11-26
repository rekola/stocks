#pragma once

// The tables of the market-data database, as sqlpp11 knows them (the typed query library both the SQLite and the MariaDB store run on;
// the DDL that makes them is in SqlStore.h, next to the differences between the two). Written by hand in the shape sqlpp11's ddl2cpp
// makes, with two macros for the parts that repeat. Keys, versions and times are 64-bit signed integers in the database (every key is
// below 2^62, see NodeKey.h); the level is the `Level` code.
//
//   meta         name, value                    schema_version, batch (the last batch number)
//   datasets     dataset (key), code, name      what says the bars: a dataset's key is made from its code (NodeKey.h)
//   batches      version, dataset, started_at, note   one ingest run: the version its bars carry
//   instruments  the edge's four keys, code, name, asset_class, first_period
//   loads        the edge, level, version, loaded_at, bar_count, first_key, last_key, last_bar_time    what a level holds
//   bars         the edge, level, bar_key, open, high, low, close, version                              one row per bar
#include <sqlpp11/char_sequence.h>
#include <sqlpp11/column_types.h>
#include <sqlpp11/table.h>

#define MARKET_COLUMN(Struct, member, sqlName, ...)                                                   \
  struct Struct {                                                                                     \
    struct _alias_t {                                                                                 \
      static constexpr const char _literal[] = sqlName;                                               \
      using _name_t = sqlpp::make_char_sequence<sizeof(_literal), _literal>;                          \
      template <typename T>                                                                           \
      struct _member_t {                                                                              \
        T member;                                                                                     \
        T & operator()() { return member; }                                                           \
        const T & operator()() const { return member; }                                               \
      };                                                                                              \
    };                                                                                                \
    using _traits = ::sqlpp::make_traits<__VA_ARGS__>;                                                \
  };

#define MARKET_TABLE(Struct, member, sqlName, ...)                                                    \
  struct Struct : sqlpp::table_t<Struct, __VA_ARGS__> {                                               \
    using _value_type = sqlpp::no_value_t;                                                            \
    struct _alias_t {                                                                                 \
      static constexpr const char _literal[] = sqlName;                                               \
      using _name_t = sqlpp::make_char_sequence<sizeof(_literal), _literal>;                          \
      template <typename T>                                                                           \
      struct _member_t {                                                                              \
        T member;                                                                                     \
        T & operator()() { return member; }                                                           \
        const T & operator()() const { return member; }                                               \
      };                                                                                              \
    };                                                                                                \
  };

#define MARKET_REQUIRED ::sqlpp::tag::require_insert

namespace market::tables {

namespace Meta_ {
MARKET_COLUMN(Name, name, "name", ::sqlpp::text, MARKET_REQUIRED)
MARKET_COLUMN(Value, value, "value", ::sqlpp::text, MARKET_REQUIRED)
}  // namespace Meta_
MARKET_TABLE(Meta, meta, "meta", Meta_::Name, Meta_::Value)

namespace Datasets_ {
MARKET_COLUMN(Dataset, dataset, "dataset", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(Code, code, "code", ::sqlpp::text, MARKET_REQUIRED)
MARKET_COLUMN(Name, name, "name", ::sqlpp::text, MARKET_REQUIRED)
}  // namespace Datasets_
MARKET_TABLE(Datasets, datasets, "datasets", Datasets_::Dataset, Datasets_::Code, Datasets_::Name)

namespace Batches_ {
MARKET_COLUMN(Version, version, "version", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(Dataset, dataset, "dataset", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(StartedAt, startedAt, "started_at", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(Note, note, "note", ::sqlpp::text, MARKET_REQUIRED)
}  // namespace Batches_
MARKET_TABLE(Batches, batches, "batches", Batches_::Version, Batches_::Dataset, Batches_::StartedAt, Batches_::Note)

namespace Instruments_ {
MARKET_COLUMN(Base, base, "base", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(Quote, quote, "quote", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(Market, market, "market", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(Dataset, dataset, "dataset", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(Code, code, "code", ::sqlpp::text, MARKET_REQUIRED)
MARKET_COLUMN(Name, name, "name", ::sqlpp::text, MARKET_REQUIRED)
MARKET_COLUMN(AssetClass, assetClass, "asset_class", ::sqlpp::text, MARKET_REQUIRED)
MARKET_COLUMN(FirstPeriod, firstPeriod, "first_period", ::sqlpp::integer, MARKET_REQUIRED)
}  // namespace Instruments_
MARKET_TABLE(Instruments, instruments, "instruments", Instruments_::Base, Instruments_::Quote, Instruments_::Market, Instruments_::Dataset,
             Instruments_::Code, Instruments_::Name, Instruments_::AssetClass, Instruments_::FirstPeriod)

namespace Loads_ {
MARKET_COLUMN(Base, base, "base", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(Quote, quote, "quote", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(Market, market, "market", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(Dataset, dataset, "dataset", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(Level, level, "level", ::sqlpp::integer, MARKET_REQUIRED)
MARKET_COLUMN(Version, version, "version", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(LoadedAt, loadedAt, "loaded_at", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(BarCount, barCount, "bar_count", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(FirstKey, firstKey, "first_key", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(LastKey, lastKey, "last_key", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(LastBarTime, lastBarTime, "last_bar_time", ::sqlpp::bigint, MARKET_REQUIRED)
}  // namespace Loads_
MARKET_TABLE(Loads, loads, "loads", Loads_::Base, Loads_::Quote, Loads_::Market, Loads_::Dataset, Loads_::Level, Loads_::Version,
             Loads_::LoadedAt, Loads_::BarCount, Loads_::FirstKey, Loads_::LastKey, Loads_::LastBarTime)

namespace Bars_ {
MARKET_COLUMN(Base, base, "base", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(Quote, quote, "quote", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(Market, market, "market", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(Dataset, dataset, "dataset", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(Level, level, "level", ::sqlpp::integer, MARKET_REQUIRED)
MARKET_COLUMN(BarKey, barKey, "bar_key", ::sqlpp::bigint, MARKET_REQUIRED)
MARKET_COLUMN(Open, open, "open", ::sqlpp::floating_point, MARKET_REQUIRED)
MARKET_COLUMN(High, high, "high", ::sqlpp::floating_point, MARKET_REQUIRED)
MARKET_COLUMN(Low, low, "low", ::sqlpp::floating_point, MARKET_REQUIRED)
MARKET_COLUMN(Close, close, "close", ::sqlpp::floating_point, MARKET_REQUIRED)
MARKET_COLUMN(Version, version, "version", ::sqlpp::bigint, MARKET_REQUIRED)
}  // namespace Bars_
MARKET_TABLE(Bars, bars, "bars", Bars_::Base, Bars_::Quote, Bars_::Market, Bars_::Dataset, Bars_::Level, Bars_::BarKey, Bars_::Open,
             Bars_::High, Bars_::Low, Bars_::Close, Bars_::Version)

}  // namespace market::tables
