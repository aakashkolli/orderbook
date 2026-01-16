# tests/python/conftest.py
import os
import pytest

@pytest.fixture(scope="session")
def itch_path():
    path = os.environ.get("ITCH_FILE", "data/mock.NASDAQ_ITCH50")
    if not os.path.exists(path):
        pytest.skip(f"ITCH_FILE not found at {path}; run gen_mock_itch.py first")
    return path
