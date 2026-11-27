#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// A thin RAII wrapper over libmariadb's prepared statements, shaped like SQLiteCpp (bind with a 1-based index, `executeStep()`, `getColumn()`),
// so that a store for MariaDB reads like the one for SQLite. Every value is bound in binary (an integer, a double, bytes), never put in SQL
// text, and a result is fetched a row at a time from the server (the whole of it is not read first). Nothing but this file and
// MariaDb.cpp includes mysql.h.
namespace maria {

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Config {
  std::string host = "localhost";
  int port = 3306;
  std::string socket;  // a Unix socket, used for the host "localhost" where set
  std::string user, password;
  std::string database;  // empty: none selected (to make it)
};

class Connection {
 public:
  explicit Connection(const Config &);  // throws Error
  ~Connection();
  Connection(const Connection &) = delete;
  Connection & operator=(const Connection &) = delete;

  // A statement without values or a result.
  void exec(const std::string & sql);
  struct Impl;
  Impl & impl() { return *impl_; }

 private:
  std::unique_ptr<Impl> impl_;
};

// A transaction that rolls back unless it is committed.
class Transaction {
 public:
  explicit Transaction(Connection &);
  ~Transaction();
  Transaction(const Transaction &) = delete;
  Transaction & operator=(const Transaction &) = delete;
  void commit();

 private:
  Connection & db_;
  bool open_ = true;
};

// A column of the row the statement is on.
class Column {
 public:
  bool isNull() const;
  int64_t getInt64() const;
  int getInt() const { return static_cast<int>(getInt64()); }
  double getDouble() const;
  std::string getString() const;
  // The bytes of a blob or text, valid until the statement steps or is reset.
  std::string_view getBlob() const;

 private:
  friend class Statement;
  Column(const class Statement * s, unsigned i) : statement_(s), index_(i) {}
  const class Statement * statement_;
  unsigned index_;
};

class Statement {
 public:
  Statement(Connection &, const std::string & sql);  // throws Error
  ~Statement();
  Statement(const Statement &) = delete;
  Statement & operator=(const Statement &) = delete;

  // Bind the value of `?` number `index` (from 1). With no value: NULL.
  void bind(int index, int64_t value);
  void bind(int index, int value) { bind(index, static_cast<int64_t>(value)); }
  void bind(int index, double value);
  void bind(int index, const std::string & value);
  void bind(int index, const char * data, size_t size);  // a blob
  void bind(int index);

  // Runs a statement that has no result; the number of rows it changed.
  uint64_t exec();
  // Runs it on the first call and then steps through the rows: false at the end.
  bool executeStep();
  // Ends the result (what is left of it is read and dropped), so that the statement can run again with new values.
  void reset();
  Column getColumn(unsigned index) const { return Column(this, index); }
  uint64_t lastInsertId() const;

  struct Impl;
  const Impl & impl() const { return *impl_; }

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace maria
