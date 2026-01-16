# tests/python/test_bindings_smoke.py
import orderbook


def test_module_attributes():
    """Module exports required symbols."""
    assert hasattr(orderbook, "ReplaySession")
    assert hasattr(orderbook, "load_replay")
    assert hasattr(orderbook, "EventType")
    assert hasattr(orderbook, "Side")


def test_event_type_values():
    """EventType enum has all required variants (values derived from C++)."""
    for name in ("ADD", "EXECUTE", "EXECUTE_PRICE", "CANCEL", "DELETE", "REPLACE"):
        assert hasattr(orderbook.EventType, name), f"EventType.{name} missing"


def test_side_values():
    """Side enum maps to expected char codes."""
    assert int(orderbook.Side.BID) == ord('B')
    assert int(orderbook.Side.ASK) == ord('S')


def test_step_and_snapshot(itch_path):
    """Step 1000 events; snapshot has required keys and structural invariants."""
    s = orderbook.load_replay(itch_path)
    count = 0
    while s.step() and count < 1000:
        count += 1
    assert count == 1000, "expected exactly 1000 steps"

    snap = s.snapshot()
    required_keys = {"best_bid", "best_bid_volume", "best_ask",
                     "best_ask_volume", "checksum", "timestamp_ns", "order_count"}
    assert required_keys.issubset(snap.keys())
    assert snap["order_count"] > 100
    assert snap["best_bid"] is not None, "best_bid should be populated after 1000 events"
    assert snap["best_ask"] is not None, "best_ask should be populated after 1000 events"
    assert snap["best_bid"] < snap["best_ask"], "crossed market in snapshot"
    assert snap["checksum"].startswith("0x")


def test_query_top_n_bid_descending(itch_path):
    """query_top_n returns bid levels in strictly descending price order."""
    s = orderbook.load_replay(itch_path)
    for _ in range(500):
        s.step()
    levels = s.query_top_n(5, orderbook.Side.BID)
    assert len(levels) <= 5
    prices = [p for p, _ in levels]
    assert prices == sorted(prices, reverse=True), "bid prices not descending"


def test_query_top_n_ask_ascending(itch_path):
    """query_top_n returns ask levels in strictly ascending price order."""
    s = orderbook.load_replay(itch_path)
    for _ in range(500):
        s.step()
    levels = s.query_top_n(5, orderbook.Side.ASK)
    assert len(levels) <= 5
    prices = [p for p, _ in levels]
    assert prices == sorted(prices), "ask prices not ascending"


def test_register_sink_receives_fills(itch_path):
    """Sink callable receives tuples for EXECUTE and EXECUTE_PRICE events."""
    s = orderbook.load_replay(itch_path)
    fills = []
    s.register_sink(lambda t: fills.append(t))
    for _ in range(2000):
        s.step()
    assert fills, "expected at least one fill in first 2000 events"
    for t in fills:
        assert isinstance(t, tuple) and len(t) == 5
        assert t[0] in (
            orderbook.EventType.EXECUTE.value,
            orderbook.EventType.EXECUTE_PRICE.value,
        ), f"unexpected event type in sink: {t[0]}"


def test_unknown_extension_raises():
    """load_replay raises for unknown file extension."""
    import pytest
    with pytest.raises(RuntimeError, match="Unknown format"):
        orderbook.load_replay("data/file.xyz")


def test_eof_returns_false(itch_path):
    """step() returns False at EOF and remains stable."""
    s = orderbook.load_replay(itch_path)
    # Drain the file
    while s.step():
        pass
    assert not s.step()
    assert not s.step()  # idempotent
