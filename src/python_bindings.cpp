// src/python_bindings.cpp
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "csv_adapter.hpp"
#include "orderbook/event_source.hpp"
#include "orderbook/replay_session.hpp"
#ifdef HAVE_ARROW
#include "parquet_adapter.hpp"
#endif
#include <climits>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include "event.h"

namespace py = pybind11;
using namespace pybind11::literals;

// ---------------------------------------------------------------------------
// Format detection + source factory
// ---------------------------------------------------------------------------

static std::unique_ptr<IEventSource> make_source(const std::string& path) {
  auto dot = path.rfind('.');
  if (dot == std::string::npos)
    throw std::runtime_error(
        "No file extension in path: " + path +
        ". Supported: .csv, .parquet, .itch50, .NASDAQ_ITCH50, .bin");
  std::string ext = path.substr(dot);
  if (ext == ".csv") return std::make_unique<CsvEventSource>(path.c_str());
  if (ext == ".parquet") {
#ifdef HAVE_ARROW
    return std::make_unique<ParquetEventSource>(path.c_str());
#else
    throw std::runtime_error(
        "Parquet support not compiled in; "
        "rebuild with -DORDERBOOK_PYTHON=ON and Arrow/Parquet installed");
#endif
  }
  if (ext == ".itch50" || ext == ".NASDAQ_ITCH50" || ext == ".bin")
    return make_itch_source(path.c_str());
  throw std::runtime_error(
      "Unknown format: " + ext +
      ". Supported extensions: .csv, .parquet, .itch50, .NASDAQ_ITCH50, .bin");
}

// ---------------------------------------------------------------------------
// Python-side wrapper: owns ReplaySession + Python sink callable
// ---------------------------------------------------------------------------

struct PySinkState {
  py::object fn;
};

struct PyReplaySession {
  std::unique_ptr<ReplaySession> session;
  std::unique_ptr<PySinkState> sink_state;  // null until register_sink called

  static void trampoline(const Event& ev, void* ctx) {
    py::gil_scoped_acquire gil;
    auto* s = static_cast<PySinkState*>(ctx);
    // ev.side is char; Side enum values are 'B'=0x42 and 'S'=0x53, matching
    // the ITCH wire values stored in Event::side. The cast is intentional.
    s->fn(py::make_tuple(
        static_cast<int>(ev.type),      // int; compare with EventType.X.value
        ev.price,                       // int64 fixed-point ($0.0001 units)
        static_cast<int>(ev.quantity),  // int
        static_cast<Side>(ev.side),     // Side enum object via pybind binding
        ev.timestamp_ns                 // uint64 nanoseconds
        ));
  }
};

// ---------------------------------------------------------------------------
// Module definition
// ---------------------------------------------------------------------------

PYBIND11_MODULE(orderbook, m) {
  m.doc() = "NASDAQ ITCH 5.0 order book replay engine";

  py::enum_<Event::Type>(m, "EventType")
      .value("ADD", Event::Type::ADD)
      .value("EXECUTE", Event::Type::EXECUTE)
      .value("EXECUTE_PRICE", Event::Type::EXECUTE_PRICE)
      .value("CANCEL", Event::Type::CANCEL)
      .value("DELETE", Event::Type::DELETE)
      .value("REPLACE", Event::Type::REPLACE)
      .export_values();

  py::enum_<Side>(m, "Side").value("BID", Side::BID).value("ASK", Side::ASK);

  py::class_<PyReplaySession>(m, "ReplaySession")
      .def(py::init([](const std::string& path, const std::string& mode) {
             if (mode != "replay")
               throw std::invalid_argument("Unknown mode '" + mode +
                                           "'. Supported: 'replay'");
             auto prs = std::make_unique<PyReplaySession>();
             prs->session = std::make_unique<ReplaySession>(make_source(path));
             return prs;
           }),
           py::arg("path"), py::arg("mode") = "replay")

      .def("step",
           [](PyReplaySession& self) {
             py::gil_scoped_release release;
             return self.session->step();
           })

      .def("snapshot",
           [](const PyReplaySession& self) {
             Snapshot s = self.session->snapshot();
             std::ostringstream oss;
             oss << "0x" << std::hex << std::setfill('0') << std::setw(16)
                 << s.checksum;
             // Map sentinel prices to None so Python callers can detect an
             // empty side without interpreting INT64_MAX or 0 as real prices.
             py::object bid_price =
                 (s.best_bid == 0) ? py::none() : py::cast(s.best_bid);
             py::object ask_price =
                 (s.best_ask == INT64_MAX) ? py::none() : py::cast(s.best_ask);
             return py::dict("best_bid"_a = bid_price,
                             "best_bid_volume"_a = s.best_bid_volume,
                             "best_ask"_a = ask_price,
                             "best_ask_volume"_a = s.best_ask_volume,
                             "checksum"_a = oss.str(),
                             "timestamp_ns"_a = s.timestamp_ns,
                             "order_count"_a = s.order_count);
           })

      .def(
          "query_top_n",
          [](const PyReplaySession& self, int n, Side side) {
            return self.session->query_top_n(n, side);
          },
          py::arg("n"), py::arg("side"),
          "Return up to n price levels as list of (price_tick, volume) tuples. "
          "Bids are descending; asks are ascending. price_tick is in $0.0001 "
          "units.")

      .def("register_sink", [](PyReplaySession& self, py::object fn) {
        // Build new state first, register its pointer with the session, then
        // release the old state: this order ensures the session never holds a
        // pointer to a destroyed PySinkState even if step() runs concurrently.
        auto new_state = std::make_unique<PySinkState>(PySinkState{fn});
        self.session->register_sink(PyReplaySession::trampoline,
                                    new_state.get());
        self.sink_state = std::move(new_state);
      });

  m.def(
      "load_replay",
      [](const std::string& path) {
        auto prs = std::make_unique<PyReplaySession>();
        prs->session = std::make_unique<ReplaySession>(make_source(path));
        return prs;
      },
      py::arg("path"),
      "Load a replay session from path. Format auto-detected by extension "
      "(.itch50/.NASDAQ_ITCH50/.bin -> ITCH, .csv -> CSV, .parquet -> "
      "Parquet).");
}
