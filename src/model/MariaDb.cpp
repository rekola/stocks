#include "model/MariaDb.h"

#include <mysql.h>

#include <cstring>

namespace maria {

struct Connection::Impl {
  MYSQL * mysql = nullptr;
};

Connection::Connection(const Config & config) : impl_(std::make_unique<Impl>()) {
  impl_->mysql = mysql_init(nullptr);
  if (!impl_->mysql) throw Error("mariadb: cannot start the client");
  mysql_options(impl_->mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4");
  // No MYSQL_OPT_RECONNECT: a connection that is made again silently inside a transaction is in autocommit mode, and what was done before it
  // would be committed half. A connection that is lost is an error of the statement; whoever holds the Connection makes a new one.
  const bool local = config.host.empty() || config.host == "localhost";
  if (!mysql_real_connect(impl_->mysql, config.host.c_str(), config.user.c_str(), config.password.c_str(),
                          config.database.empty() ? nullptr : config.database.c_str(), static_cast<unsigned>(config.port),
                          local && !config.socket.empty() ? config.socket.c_str() : nullptr, 0)) {
    const std::string why = mysql_error(impl_->mysql);
    mysql_close(impl_->mysql);
    throw Error("mariadb: cannot connect to " + config.host + " as " + config.user + ": " + why);
  }
}

Connection::~Connection() {
  if (impl_ && impl_->mysql) mysql_close(impl_->mysql);
}

void Connection::exec(const std::string & sql) {
  if (mysql_real_query(impl_->mysql, sql.data(), static_cast<unsigned long>(sql.size())) != 0)
    throw Error(std::string("mariadb: ") + mysql_error(impl_->mysql) + " [" + sql.substr(0, 100) + "]");
}

Transaction::Transaction(Connection & db) : db_(db) { db_.exec("START TRANSACTION"); }
Transaction::~Transaction() {
  if (!open_) return;
  try {
    db_.exec("ROLLBACK");
  } catch (...) {
  }
}
void Transaction::commit() {
  db_.exec("COMMIT");
  open_ = false;
}

// What one column of a result is read into.
struct Cell {
  enum Kind { Integer, Real, Bytes } kind = Bytes;
  int64_t integer = 0;
  double real = 0;
  std::string bytes;
  unsigned long length = 0;
  my_bool isNull = 0, error = 0;
};

struct Statement::Impl {
  Connection * connection = nullptr;
  MYSQL_STMT * stmt = nullptr;
  std::vector<MYSQL_BIND> params;
  // What was bound, kept alive until the statement runs.
  std::vector<int64_t> ints;
  std::vector<double> reals;
  std::vector<std::string> texts;
  std::vector<my_bool> nulls;
  std::vector<unsigned long> lengths;
  std::vector<Cell> cells;
  std::vector<MYSQL_BIND> results;
  bool executed = false, hasResult = false;
};

namespace {

[[noreturn]] void fail(MYSQL_STMT * stmt, const char * what) { throw Error(std::string("mariadb: ") + what + ": " + mysql_stmt_error(stmt)); }

}  // namespace

Statement::Statement(Connection & connection, const std::string & sql) : impl_(std::make_unique<Impl>()) {
  impl_->connection = &connection;
  impl_->stmt = mysql_stmt_init(connection.impl().mysql);
  if (!impl_->stmt) throw Error("mariadb: cannot make a statement");
  if (mysql_stmt_prepare(impl_->stmt, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
    const std::string why = mysql_stmt_error(impl_->stmt);
    mysql_stmt_close(impl_->stmt);
    throw Error("mariadb: " + why + " [" + sql.substr(0, 100) + "]");
  }
  const size_t count = mysql_stmt_param_count(impl_->stmt);
  impl_->params.assign(count, MYSQL_BIND{});
  impl_->ints.assign(count, 0);
  impl_->reals.assign(count, 0);
  impl_->texts.assign(count, {});
  impl_->nulls.assign(count, 1);
  impl_->lengths.assign(count, 0);
  for (size_t i = 0; i < count; i++) {
    impl_->params[i].buffer_type = MYSQL_TYPE_NULL;
    impl_->params[i].is_null = &impl_->nulls[i];
  }
}

Statement::~Statement() {
  if (impl_ && impl_->stmt) mysql_stmt_close(impl_->stmt);
}

namespace {
size_t slot(Statement::Impl & impl, int index) {
  if (index < 1 || static_cast<size_t>(index) > impl.params.size()) throw Error("mariadb: there is no ? number " + std::to_string(index));
  return static_cast<size_t>(index - 1);
}
}  // namespace

void Statement::bind(int index, int64_t value) {
  const size_t i = slot(*impl_, index);
  impl_->ints[i] = value;
  impl_->nulls[i] = 0;
  impl_->params[i].buffer_type = MYSQL_TYPE_LONGLONG;
  impl_->params[i].buffer = &impl_->ints[i];
  impl_->params[i].is_unsigned = 0;
}
void Statement::bind(int index, double value) {
  const size_t i = slot(*impl_, index);
  impl_->reals[i] = value;
  impl_->nulls[i] = 0;
  impl_->params[i].buffer_type = MYSQL_TYPE_DOUBLE;
  impl_->params[i].buffer = &impl_->reals[i];
}
void Statement::bind(int index, const std::string & value) { bind(index, value.data(), value.size()); }
void Statement::bind(int index, const char * data, size_t size) {
  const size_t i = slot(*impl_, index);
  impl_->texts[i].assign(data, size);
  impl_->lengths[i] = static_cast<unsigned long>(size);
  impl_->nulls[i] = 0;
  impl_->params[i].buffer_type = MYSQL_TYPE_STRING;  // the bytes as they are: a blob column takes them as they are
  impl_->params[i].buffer = impl_->texts[i].data();
  impl_->params[i].buffer_length = static_cast<unsigned long>(size);
  impl_->params[i].length = &impl_->lengths[i];
}
void Statement::bind(int index) {
  const size_t i = slot(*impl_, index);
  impl_->nulls[i] = 1;
  impl_->params[i].buffer_type = MYSQL_TYPE_NULL;
}

void Statement::reset() {
  if (impl_->hasResult) mysql_stmt_free_result(impl_->stmt);
  mysql_stmt_reset(impl_->stmt);
  impl_->executed = false;
  impl_->hasResult = false;
}

uint64_t Statement::exec() {
  if (!impl_->params.empty() && mysql_stmt_bind_param(impl_->stmt, impl_->params.data()) != 0) fail(impl_->stmt, "cannot bind");
  if (mysql_stmt_execute(impl_->stmt) != 0) fail(impl_->stmt, "cannot execute");
  const uint64_t changed = mysql_stmt_affected_rows(impl_->stmt);
  mysql_stmt_reset(impl_->stmt);
  return changed;
}

uint64_t Statement::lastInsertId() const { return mysql_stmt_insert_id(impl_->stmt); }

bool Statement::executeStep() {
  Impl & s = *impl_;
  if (!s.executed) {
    if (!s.params.empty() && mysql_stmt_bind_param(s.stmt, s.params.data()) != 0) fail(s.stmt, "cannot bind");
    if (mysql_stmt_execute(s.stmt) != 0) fail(s.stmt, "cannot execute");
    s.executed = true;
    MYSQL_RES * meta = mysql_stmt_result_metadata(s.stmt);
    if (!meta) return false;  // no result
    const unsigned count = mysql_num_fields(meta);
    s.hasResult = true;
    s.cells.assign(count, Cell{});
    s.results.assign(count, MYSQL_BIND{});
    for (unsigned i = 0; i < count; i++) {
      const MYSQL_FIELD * field = mysql_fetch_field_direct(meta, i);
      Cell & cell = s.cells[i];
      MYSQL_BIND & bind = s.results[i];
      switch (field->type) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
          cell.kind = Cell::Integer;
          bind.buffer_type = MYSQL_TYPE_LONGLONG;
          bind.buffer = &cell.integer;
          bind.buffer_length = sizeof cell.integer;
          break;
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
          cell.kind = Cell::Real;
          bind.buffer_type = MYSQL_TYPE_DOUBLE;
          bind.buffer = &cell.real;
          bind.buffer_length = sizeof cell.real;
          break;
        default:
          cell.kind = Cell::Bytes;
          cell.bytes.resize(256);
          bind.buffer_type = MYSQL_TYPE_STRING;
          bind.buffer = cell.bytes.data();
          bind.buffer_length = static_cast<unsigned long>(cell.bytes.size());
          break;
      }
      bind.length = &cell.length;
      bind.is_null = &cell.isNull;
      bind.error = &cell.error;
    }
    mysql_free_result(meta);
    if (mysql_stmt_bind_result(s.stmt, s.results.data()) != 0) fail(s.stmt, "cannot bind the result");
  }
  if (!s.hasResult) return false;
  const int status = mysql_stmt_fetch(s.stmt);
  if (status == MYSQL_NO_DATA) return false;
  if (status == 1) fail(s.stmt, "cannot fetch");
  if (status == MYSQL_DATA_TRUNCATED) {
    // A column longer than what was ready for it: read again into room of its size, and keep that room for the rows after it. The bind of the
    // column must then point at the new buffer (the old one is freed by the resize), or the next row is written into freed memory.
    bool grown = false;
    for (size_t i = 0; i < s.cells.size(); i++) {
      Cell & cell = s.cells[i];
      if (cell.kind != Cell::Bytes || cell.isNull || cell.length <= cell.bytes.size()) continue;
      cell.bytes.resize(cell.length);
      MYSQL_BIND one{};
      one.buffer_type = MYSQL_TYPE_STRING;
      one.buffer = cell.bytes.data();
      one.buffer_length = cell.length;
      unsigned long length = 0;
      one.length = &length;
      if (mysql_stmt_fetch_column(s.stmt, &one, static_cast<unsigned>(i), 0) != 0) fail(s.stmt, "cannot fetch a long column");
      s.results[i].buffer = cell.bytes.data();
      s.results[i].buffer_length = static_cast<unsigned long>(cell.bytes.size());
      grown = true;
    }
    if (grown && mysql_stmt_bind_result(s.stmt, s.results.data()) != 0) fail(s.stmt, "cannot bind the result again");
  }
  return true;
}

bool Column::isNull() const { return statement_->impl().cells[index_].isNull != 0; }
int64_t Column::getInt64() const {
  const Cell & c = statement_->impl().cells[index_];
  if (c.isNull) return 0;
  if (c.kind == Cell::Integer) return c.integer;
  if (c.kind == Cell::Real) return static_cast<int64_t>(c.real);
  return std::strtoll(std::string(c.bytes.data(), c.length).c_str(), nullptr, 10);
}
double Column::getDouble() const {
  const Cell & c = statement_->impl().cells[index_];
  if (c.isNull) return 0;
  if (c.kind == Cell::Real) return c.real;
  if (c.kind == Cell::Integer) return static_cast<double>(c.integer);
  return std::strtod(std::string(c.bytes.data(), c.length).c_str(), nullptr);
}
std::string Column::getString() const {
  const Cell & c = statement_->impl().cells[index_];
  if (c.isNull) return {};
  if (c.kind == Cell::Bytes) return std::string(c.bytes.data(), c.length);
  return c.kind == Cell::Integer ? std::to_string(c.integer) : std::to_string(c.real);
}
std::string_view Column::getBlob() const {
  const Cell & c = statement_->impl().cells[index_];
  if (c.isNull || c.kind != Cell::Bytes) return {};
  return std::string_view(c.bytes.data(), c.length);
}

}  // namespace maria
