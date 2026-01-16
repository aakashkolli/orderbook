// adapters/parquet_adapter.cpp
#include "parquet_adapter.hpp"
#include "event.h"
#include "types.h"
#include <stdexcept>
#include <string>

#ifndef HAVE_ARROW

ParquetEventSource::ParquetEventSource(const char* /*path*/) {
    throw std::runtime_error(
        "Parquet support not compiled in; "
        "rebuild with -DORDERBOOK_PYTHON=ON and Arrow/Parquet installed");
}
ParquetEventSource::~ParquetEventSource() = default;
bool ParquetEventSource::next(Event& /*ev*/) noexcept { return false; }

#else   // HAVE_ARROW

#include <arrow/array.h>
#include <arrow/table.h>
#include <parquet/arrow/reader.h>

static Event::Type parse_type_str(const std::string& s) noexcept {
    if (s == "ADD")           return Event::Type::ADD;
    if (s == "EXECUTE")       return Event::Type::EXECUTE;
    if (s == "EXECUTE_PRICE") return Event::Type::EXECUTE_PRICE;
    if (s == "CANCEL")        return Event::Type::CANCEL;
    if (s == "DELETE")        return Event::Type::DELETE;
    if (s == "REPLACE")       return Event::Type::REPLACE;
    return Event::Type::UNKNOWN;
}

void ParquetEventSource::resolve_columns(const arrow::Schema& schema) {
    auto require_str = [&](const char* name) -> int {
        int idx = schema.GetFieldIndex(name);
        if (idx < 0)
            throw std::runtime_error(
                std::string("ParquetEventSource: missing column '") + name + "'");
        auto type_id = schema.field(idx)->type()->id();
        if (type_id != arrow::Type::STRING && type_id != arrow::Type::LARGE_STRING)
            throw std::runtime_error(
                std::string("ParquetEventSource: column '") + name
                + "' must be STRING, got " + schema.field(idx)->type()->ToString());
        return idx;
    };
    auto require_i64 = [&](const char* name) -> int {
        int idx = schema.GetFieldIndex(name);
        if (idx < 0)
            throw std::runtime_error(
                std::string("ParquetEventSource: missing column '") + name + "'");
        if (schema.field(idx)->type()->id() != arrow::Type::INT64)
            throw std::runtime_error(
                std::string("ParquetEventSource: column '") + name
                + "' must be INT64, got " + schema.field(idx)->type()->ToString());
        return idx;
    };
    col_type_  = require_str("type");
    col_side_  = require_str("side");
    col_price_ = require_i64("price");
    col_qty_   = require_i64("quantity");
    col_ref_   = require_i64("order_ref");
    col_ts_    = require_i64("timestamp_ns");
}

ParquetEventSource::ParquetEventSource(const char* path) {
    auto infile_result = arrow::io::ReadableFile::Open(path);
    if (!infile_result.ok())
        throw std::runtime_error("ParquetEventSource: cannot open " + std::string(path));

    std::unique_ptr<parquet::arrow::FileReader> file_reader;
    auto st = parquet::arrow::OpenFile(
        *infile_result, arrow::default_memory_pool(), &file_reader);
    if (!st.ok())
        throw std::runtime_error("ParquetEventSource: " + st.ToString());

    std::shared_ptr<arrow::RecordBatchReader> batch_reader;
    st = file_reader->GetRecordBatchReader(&batch_reader);
    if (!st.ok())
        throw std::runtime_error("ParquetEventSource: " + st.ToString());

    reader_ = batch_reader;
    resolve_columns(*reader_->schema());
    advance_batch();  // prime first batch
}

ParquetEventSource::~ParquetEventSource() = default;

bool ParquetEventSource::advance_batch() noexcept {
    try {
        auto st = reader_->ReadNext(&batch_);
        if (!st.ok() || !batch_) { batch_ = nullptr; return false; }
        row_idx_ = 0;
        return true;
    } catch (...) { batch_ = nullptr; return false; }
}

bool ParquetEventSource::next(Event& ev) noexcept {
    try {
        while (true) {
            if (!batch_ || row_idx_ >= batch_->num_rows()) {
                if (!advance_batch()) return false;
            }

            int64_t r = row_idx_++;

            auto& type_col = static_cast<const arrow::StringArray&>(
                                 *batch_->column(col_type_));
            auto& side_col = static_cast<const arrow::StringArray&>(
                                 *batch_->column(col_side_));
            auto& price_col= static_cast<const arrow::Int64Array&>(
                                 *batch_->column(col_price_));
            auto& qty_col  = static_cast<const arrow::Int64Array&>(
                                 *batch_->column(col_qty_));
            auto& ref_col  = static_cast<const arrow::Int64Array&>(
                                 *batch_->column(col_ref_));
            auto& ts_col   = static_cast<const arrow::Int64Array&>(
                                 *batch_->column(col_ts_));

            Event::Type t = parse_type_str(type_col.GetString(r));
            if (t == Event::Type::UNKNOWN) continue;
            // REPLACE requires new_order_ref, which has no dedicated Parquet column.
            // Skipping REPLACE rows is preferable to silently corrupting the book by
            // passing new_order_ref=0 (all replacements would alias to the same ref).
            if (t == Event::Type::REPLACE) continue;

            ev = {};
            ev.type         = t;
            std::string side_str = side_col.GetString(r);
            ev.side         = side_str.empty() ? '\0' : side_str[0];
            ev.price        = static_cast<Price>(price_col.Value(r));
            ev.quantity     = static_cast<Quantity>(qty_col.Value(r));
            ev.order_ref    = static_cast<OrderRef>(ref_col.Value(r));
            ev.timestamp_ns = static_cast<Timestamp>(ts_col.Value(r));
            // ev.new_order_ref: intentionally left 0; REPLACE rows are skipped above.
            // ev.stock_locate: intentionally left 0: Parquet files are assumed single-symbol.
            return true;
        }
    } catch (...) { return false; }
}

#endif  // HAVE_ARROW
