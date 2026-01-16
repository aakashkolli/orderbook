// adapters/csv_adapter.hpp
#pragma once
#include "orderbook/event_source.hpp"
#include <fstream>
#include <string>

struct CsvColumnMap {
    int  type         = 0;
    int  side         = 1;
    int  price        = 2;
    int  quantity     = 3;
    int  order_ref    = 4;
    int  timestamp_ns = 5;
    char delimiter    = ',';
};

class CsvEventSource : public IEventSource {
public:
    // Throws std::runtime_error if path cannot be opened.
    explicit CsvEventSource(const char* path, CsvColumnMap cols = {});

    // Returns true when ev is populated. Returns false on EOF or unrecoverable error.
    // Skips rows with UNKNOWN type (e.g., header lines) silently.
    bool next(Event& ev) noexcept override;

private:
    std::ifstream file_;
    CsvColumnMap  cols_;
    int           max_col_;  // precomputed from cols_ at construction
};
