// adapters/csv_adapter.cpp
#include "csv_adapter.hpp"
#include "event.h"
#include "types.h"
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <cmath>

// Split string_view on delimiter: no allocation for the view itself.
static std::vector<std::string_view> split_sv(std::string_view sv, char delim) {
    std::vector<std::string_view> out;
    while (true) {
        auto pos = sv.find(delim);
        out.push_back(sv.substr(0, pos));
        if (pos == std::string_view::npos) break;
        sv.remove_prefix(pos + 1);
    }
    return out;
}

static Event::Type parse_type(std::string_view s) noexcept {
    if (s == "ADD")           return Event::Type::ADD;
    if (s == "EXECUTE")       return Event::Type::EXECUTE;
    if (s == "EXECUTE_PRICE") return Event::Type::EXECUTE_PRICE;
    if (s == "CANCEL")        return Event::Type::CANCEL;
    if (s == "DELETE")        return Event::Type::DELETE;
    if (s == "REPLACE")       return Event::Type::REPLACE;
    return Event::Type::UNKNOWN;
}

// Accepts decimal strings ("50.0100" -> 500100) or pre-scaled integer ticks ("500100" -> 500100).
// Decimal presence is detected by searching for '.'; integer strings bypass the ×10000 scaling.
static Price parse_price(std::string_view s) {
    if (s.find('.') != std::string_view::npos) {
        double d = std::stod(std::string(s));
        return static_cast<Price>(std::llround(d * 10000.0));
    }
    return static_cast<Price>(std::stoll(std::string(s)));
}

CsvEventSource::CsvEventSource(const char* path, CsvColumnMap cols)
    : file_(path), cols_(cols)
    , max_col_(std::max({cols.type, cols.side, cols.price,
                         cols.quantity, cols.order_ref, cols.timestamp_ns}))
{
    if (!file_.is_open())
        throw std::runtime_error(std::string("CsvEventSource: cannot open ") + path);
}

bool CsvEventSource::next(Event& ev) noexcept {
    try {
        std::string line;
        while (std::getline(file_, line)) {
            if (line.empty()) continue;
            if (line.back() == '\r') line.pop_back();  // trim Windows line endings

            auto fields = split_sv(std::string_view(line), cols_.delimiter);
            if (static_cast<int>(fields.size()) <= max_col_) continue;

            Event::Type t = parse_type(fields[cols_.type]);
            if (t == Event::Type::UNKNOWN) continue;  // skip header/comment rows

            ev = {};
            ev.type         = t;
            ev.side         = fields[cols_.side].empty() ? '\0' : fields[cols_.side][0];
            ev.price        = parse_price(fields[cols_.price]);
            ev.quantity     = static_cast<Quantity>(
                                  std::stoull(std::string(fields[cols_.quantity])));
            ev.order_ref    = static_cast<OrderRef>(
                                  std::stoull(std::string(fields[cols_.order_ref])));
            ev.timestamp_ns = static_cast<Timestamp>(
                                  std::stoull(std::string(fields[cols_.timestamp_ns])));
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}
