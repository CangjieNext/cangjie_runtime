#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generate 4 complex JSON payload files for case27.

Target sizes: 1K, 10K, 100K, 1M.

Each payload is a deeply-nested document with mixed types (int/float/bool/null/
string/array/object). Sizes are controlled by the count of repeated subtrees
("events"), NOT by string padding. JSON structure stays the same across sizes
so the encode/decode hot paths exercise all type branches identically.

Run:
    python3 gen.py
Writes payloads/1K.json, 10K.json, 100K.json, 1M.json next to this
script.
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Each "event" object is roughly ~600 bytes after json.dumps(separators=(',',':')).
# Tweak EVENT_COUNTS to land near the target file size.
TARGETS = [
    ("1K.json",    1),
    ("10K.json",   8),
    ("100K.json",  85),
    ("1M.json",    870),
]


def make_event(i: int) -> dict:
    """Single event ~600B. Mixed types, 4-level nesting, arrays of objects."""
    return {
        "event_id": f"evt-{i:08d}",
        "seq": i,
        "ts": 1714560000000 + i * 1000,
        "type": ["page_view", "click", "purchase", "scroll"][i % 4],
        "ok": (i % 7) != 0,
        "score": round(0.001 * (i % 1000), 4),
        "tags": ["alpha", "beta", "gamma"][: (i % 3) + 1],
        "user": {
            "uid": f"u-{i % 1000}",
            "anonymous": (i % 13) == 0,
            "segment": ["premium", "standard", "trial"][i % 3],
            "age_group": ["18-24", "25-34", "35-44", "45+"][i % 4],
            "country": "CN",
            "scores": [i % 100, (i * 7) % 100, (i * 31) % 100],
            "attrs": {
                "vip_level": i % 5,
                "credits": i * 1.5,
                "verified": (i % 2) == 1,
                "since": 1700000000 + i * 60,
                "labels": {
                    "primary": f"label-{i % 17}",
                    "secondary": f"label-{i % 23}",
                    "weights": [0.1, 0.4, 0.5],
                },
            },
        },
        "page": {
            "path": f"/catalog/p/{i}",
            "title": f"Item {i} detail page",
            "depth": i % 7,
            "load_ms": 150 + (i * 3) % 500,
            "metrics": {
                "fcp": 180 + i % 300,
                "lcp": 600 + i % 800,
                "cls": round(0.01 * (i % 10), 3),
                "fid": 5 + i % 30,
                "ttfb": 40 + i % 100,
            },
            "ab": {
                "variant": "control" if (i % 2) == 0 else "treatment-v2",
                "exp_id": f"exp-{1000 + i % 10}",
                "weight": round(0.05 * (i % 20), 3),
            },
        },
        "device": {
            "type": ["mobile", "desktop", "tablet"][i % 3],
            "os": "iOS" if (i % 2) == 0 else "Android",
            "os_ver": "17.4.1" if (i % 2) == 0 else "14.2",
            "screen": ["390x844", "1440x900", "810x1080"][i % 3],
            "ratio": 2.0,
            "online": True,
            "battery": None if (i % 5) == 0 else (i % 100) / 100.0,
        },
        "geo": {
            "city": ["Shanghai", "Beijing", "Shenzhen", "Hangzhou", "Chengdu"][i % 5],
            "lat": round(31.0 + (i % 10) * 0.1, 4),
            "lon": round(121.0 + (i % 10) * 0.1, 4),
            "accuracy_m": (i % 50) + 5,
            "isp": ["CMCC", "CTCC", "CUCC"][i % 3],
        },
        "custom": {
            "flags": [
                "feature-search-v2",
                "feature-rec-engine",
                "feature-chat",
            ][: (i % 3) + 1],
            "product_ids": [i, i + 100, i + 200, i + 300],
            "funnel_step": i % 5 + 1,
            "utm": {
                "source": "organic",
                "medium": "search",
                "campaign": "spring-sale-2026",
                "content": f"ad-{i % 50}",
                "term": f"kw-{i % 200}",
            },
            "extra": {
                "weights": [0.1 + 0.01 * j for j in range(5)],
                "matrix": [[i % 3, i % 5], [i % 7, i % 11], [i % 13, i % 17]],
                "deep": {
                    "level1": {
                        "level2": {
                            "level3": {
                                "value": i * 31337 % 100000,
                                "ratio": round((i % 100) / 100.0, 4),
                            }
                        }
                    }
                },
            },
        },
    }


def build_document(event_count: int) -> dict:
    return {
        "request_id": "req-case27",
        "schema_version": "3.1.0",
        "pipeline": "analytics-ingest-v2",
        "ingestion_time": 1714560000999,
        "config": {
            "region": "cn-east-1",
            "batch_id": "batch-case27",
            "compression": False,
            "encryption": None,
            "checksum_algo": "sha256",
            "thresholds": {"warn": 0.7, "error": 0.95, "fatal": 0.99},
        },
        "counters": {"in": 0, "ok": 0, "err": 0, "retry": 0},
        "events": [make_event(i) for i in range(event_count)],
    }


def main() -> int:
    for name, n in TARGETS:
        doc = build_document(n)
        out = HERE / name
        text = json.dumps(doc, ensure_ascii=False, separators=(",", ":"))
        out.write_text(text, encoding="utf-8")
        size = out.stat().st_size
        print(f"[gen] {name:>10s}  events={n:>5d}  size={size:>10d}B  ({size/1024:.1f}KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
