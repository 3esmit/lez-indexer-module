#!/usr/bin/env python3
"""Validate the deployed Cryptarchia block response contract."""

import json
from pathlib import Path


FIXTURE = Path(__file__).parent / "fixtures" / "deployed_block_root_payload.json"


def main() -> None:
    payload = json.loads(FIXTURE.read_text(encoding="utf-8"))
    assert isinstance(payload, list) and payload, "response must contain a block"

    block = payload[0]
    header = block["header"]
    assert header["slot"] == 348, "fixture must cover the first deployed non-empty block"
    assert "block_root" in header, "deployed response must retain block_root"
    assert "body_root" not in header, "fixture must represent the deployed field name"
    assert len(header["block_root"]) == 64, "block_root must be a 32-byte hex value"
    assert isinstance(block["transactions"], list), "transactions must be an array"


if __name__ == "__main__":
    main()
