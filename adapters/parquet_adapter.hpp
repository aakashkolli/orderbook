// adapters/parquet_adapter.hpp
#pragma once
#include "orderbook/event_source.hpp"
#include <memory>
#include <stdexcept>

#ifdef HAVE_ARROW
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#endif

class ParquetEventSource : public IEventSource {
public:
    // Throws std::runtime_error if:
    //   - Arrow not compiled in (HAVE_ARROW not defined)
    //   - File cannot be opened
    //   - Schema is missing required columns
    explicit ParquetEventSource(const char* path);
    ~ParquetEventSource() override;

    bool next(Event& ev) noexcept override;

private:
#ifdef HAVE_ARROW
    std::shared_ptr<arrow::RecordBatchReader> reader_;
    std::shared_ptr<arrow::RecordBatch>       batch_;
    int64_t row_idx_{0};

    // Column indices (resolved once at construction from schema)
    int col_type_{-1}, col_side_{-1}, col_price_{-1},
        col_qty_{-1},  col_ref_{-1},  col_ts_{-1};

    void resolve_columns(const arrow::Schema& schema);
    bool advance_batch() noexcept;
#endif
};
