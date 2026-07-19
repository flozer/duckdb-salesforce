#!/usr/bin/env python3
"""Update the cumulative DuckDB Community downloads badge payload."""

from __future__ import annotations

import json
from concurrent.futures import ThreadPoolExecutor
from datetime import date, timedelta
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen


EXTENSION = "salesforce"
FIRST_STATS_SUNDAY = date(2026, 1, 4)
STATS_URL = (
    "https://community-extensions.duckdb.org/"
    "download-stats-weekly/{year}/{week}.json"
)
OUTPUT = Path(__file__).resolve().parents[1] / ".github/badges/downloads.json"


def weekly_urls(today: date) -> list[str]:
    urls: list[str] = []
    sunday = FIRST_STATS_SUNDAY
    while sunday <= today:
        iso_year, iso_week, _ = sunday.isocalendar()
        if iso_week != 53:
            urls.append(STATS_URL.format(year=iso_year, week=iso_week))
        sunday += timedelta(days=7)
    return urls


def fetch_week(url: str) -> tuple[int, str | None] | None:
    request = Request(url, headers={"User-Agent": "duckdb-salesforce-badge"})
    try:
        with urlopen(request, timeout=30) as response:
            payload = json.load(response)
    except HTTPError as error:
        if error.code == 404:
            return None
        raise

    downloads = payload.get(EXTENSION)
    return (int(downloads) if downloads is not None else 0, payload.get("_last_update"))


def main() -> None:
    with ThreadPoolExecutor(max_workers=8) as executor:
        weeks = list(executor.map(fetch_week, weekly_urls(date.today())))

    available = [week for week in weeks if week is not None]
    total = sum(downloads for downloads, _ in available)
    last_updates = [updated for _, updated in available if updated]
    if not last_updates:
        raise RuntimeError("No DuckDB Community download statistics were available")

    badge = {
        "schemaVersion": 1,
        "label": "downloads",
        "message": f"{total:,}",
        "color": "blue",
        "cacheSeconds": 3600,
    }
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(badge, indent=2) + "\n", encoding="utf-8")
    print(f"{EXTENSION}: {total:,} downloads through {max(last_updates)}")


if __name__ == "__main__":
    main()
