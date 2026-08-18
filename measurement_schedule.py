#!/usr/bin/env python3
"""
measurement_schedule.py
-----------------------
Shows the scheduled measurement times for a given day, using the same
sunrise/sunset algorithm as the weather station firmware (weather_station_full.ino).

Usage:
    python measurement_schedule.py
        → interactive prompts for session start, uses today as target day

    python measurement_schedule.py --start "2026-07-04 07:00" --day 2026-07-15
        → non-interactive, specific target day

    python measurement_schedule.py --start "2026-07-04 07:00" --day today
        → non-interactive, today as target day

Configuration (mirrors firmware defines):
    STATION_LAT          = 47.33
    STATION_LON          = 8.58
    DAILY_MEASUREMENTS   = 20
    SUNRISE_BUFFER_MIN   = 30
    SUNSET_BUFFER_MIN    = 5
"""

import argparse
import math
from datetime import date, datetime, timedelta


# ── Station configuration (must match firmware) ────────────────────────────────
STATION_LAT         = 47.33
STATION_LON         = 8.58
DAILY_MEASUREMENTS  = 20
SUNRISE_BUFFER_MIN  = 30
SUNSET_BUFFER_MIN   = 5


# ── Astronomical helpers (exact port from firmware) ────────────────────────────

def is_european_dst(year: int, month: int, day: int) -> bool:
    """True if the date falls inside European Summer Time (UTC+2)."""
    if month < 3 or month > 10:
        return False
    if 3 < month < 10:
        return True
    import calendar
    # Last Sunday of the month
    last_day = calendar.monthrange(year, month)[1]
    # Weekday of last day: 0=Monday … 6=Sunday
    wd = date(year, month, last_day).weekday()  # 0=Mon, 6=Sun
    last_sun = last_day - (wd + 1) % 7          # subtract days to reach Sunday
    if month == 3:
        return day >= last_sun
    else:  # month == 10
        return day < last_sun


def day_of_year(d: date) -> int:
    return d.timetuple().tm_yday


def sunrise_sunset(doy: int, lat: float, lon: float, tz_hours: float):
    """
    Returns (sunrise_min, sunset_min) as minutes from midnight (local time).
    NOAA simplified algorithm — matches firmware implementation exactly.
    """
    gamma = 2.0 * math.pi * (doy - 1) / 365.0

    eq = 229.18 * (
        0.000075
        + 0.001868 * math.cos(gamma)   - 0.032077 * math.sin(gamma)
        - 0.014615 * math.cos(2*gamma) - 0.040890 * math.sin(2*gamma)
    )

    decl = (
        0.006918
        - 0.399912 * math.cos(gamma)   + 0.070257 * math.sin(gamma)
        - 0.006758 * math.cos(2*gamma) + 0.000907 * math.sin(2*gamma)
        - 0.002697 * math.cos(3*gamma) + 0.001480 * math.sin(3*gamma)
    )

    deg = math.pi / 180.0
    cos_ha = (
        math.cos(90.833 * deg) / (math.cos(lat * deg) * math.cos(decl))
        - math.tan(lat * deg) * math.tan(decl)
    )
    cos_ha = max(-1.0, min(1.0, cos_ha))
    ha = math.acos(cos_ha)

    sunrise_min = round(720.0 - 4.0 * (lon + math.degrees(ha)) - eq + tz_hours * 60.0)
    sunset_min  = round(720.0 - 4.0 * (lon - math.degrees(ha)) - eq + tz_hours * 60.0)
    return sunrise_min, sunset_min


def build_schedule(target: date, n_measurements: int = DAILY_MEASUREMENTS):
    """
    Returns a list of (hh, mm) tuples for scheduled measurements on `target`.
    """
    tz = 2.0 if is_european_dst(target.year, target.month, target.day) else 1.0
    doy = day_of_year(target)

    sr_min, ss_min = sunrise_sunset(doy, STATION_LAT, STATION_LON, tz)

    play_start = sr_min + SUNRISE_BUFFER_MIN
    play_end   = ss_min - SUNSET_BUFFER_MIN

    if play_end <= play_start:
        return [], sr_min, ss_min, tz

    slots = []
    n = n_measurements
    for i in range(n):
        if n == 1:
            m = (play_start + play_end) // 2
        else:
            m = play_start + i * (play_end - play_start) // (n - 1)
        slots.append((m // 60, m % 60))

    return slots, sr_min, ss_min, tz


def fmt_min(total_minutes: int) -> str:
    return f"{total_minutes // 60:02d}:{total_minutes % 60:02d}"


# ── Main ───────────────────────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(
        description="Show weather-station measurement schedule for a given day."
    )
    p.add_argument(
        "--start", "-s",
        metavar="DATETIME",
        help='Session start as "YYYY-MM-DD HH:MM" (e.g. "2026-07-04 07:00")',
    )
    p.add_argument(
        "--day", "-d",
        metavar="DATE",
        help='Target day as YYYY-MM-DD or "today" (default: today)',
    )
    p.add_argument(
        "--measurements", "-n",
        type=int,
        default=DAILY_MEASUREMENTS,
        help=f"Number of daily measurements (default: {DAILY_MEASUREMENTS})",
    )
    return p.parse_args()


def prompt_session_start() -> datetime:
    print("\nEnter session start date and time.")
    while True:
        raw = input("  Session start [YYYY-MM-DD HH:MM]: ").strip()
        try:
            return datetime.strptime(raw, "%Y-%m-%d %H:%M")
        except ValueError:
            print("  ✗ Invalid format. Use YYYY-MM-DD HH:MM (e.g. 2026-07-04 07:00)")


def prompt_target_day() -> date:
    today_str = date.today().isoformat()
    raw = input(f"  Target day [YYYY-MM-DD or Enter for today ({today_str})]: ").strip()
    if not raw or raw.lower() == "today":
        return date.today()
    try:
        return date.fromisoformat(raw)
    except ValueError:
        print(f"  ✗ Invalid date '{raw}', using today.")
        return date.today()


def main():
    args = parse_args()

    # ── Session start ──────────────────────────────────────────────────────────
    if args.start:
        try:
            session_start = datetime.strptime(args.start, "%Y-%m-%d %H:%M")
        except ValueError:
            print(f"✗ Invalid --start value '{args.start}'. Use YYYY-MM-DD HH:MM.")
            raise SystemExit(1)
    else:
        session_start = prompt_session_start()

    # ── Target day ─────────────────────────────────────────────────────────────
    if args.day:
        if args.day.lower() == "today":
            target = date.today()
        else:
            try:
                target = date.fromisoformat(args.day)
            except ValueError:
                print(f"✗ Invalid --day value '{args.day}'. Use YYYY-MM-DD.")
                raise SystemExit(1)
    else:
        print()
        target = prompt_target_day()

    n_meas = args.measurements

    # ── Validate: target must be on or after session start ─────────────────────
    if target < session_start.date():
        print(f"\n✗ Target day {target} is before session start {session_start.date()}.")
        raise SystemExit(1)

    days_elapsed = (target - session_start.date()).days

    # ── Build schedule ─────────────────────────────────────────────────────────
    slots, sr_min, ss_min, tz = build_schedule(target, n_meas)

    # ── Output ─────────────────────────────────────────────────────────────────
    print()
    print("=" * 48)
    print(f"  Weather Station — Measurement Schedule")
    print("=" * 48)
    print(f"  Session start : {session_start.strftime('%Y-%m-%d %H:%M')}")
    print(f"  Target day    : {target.isoformat()}  (day {days_elapsed + 1} of session)")
    print(f"  Timezone      : UTC+{tz:.0f} ({'CEST/summer' if tz == 2.0 else 'CET/winter'})")
    print(f"  Sunrise       : {fmt_min(sr_min)}  →  window start: {fmt_min(sr_min + SUNRISE_BUFFER_MIN)}")
    print(f"  Sunset        : {fmt_min(ss_min)}  →  window end  : {fmt_min(ss_min - SUNSET_BUFFER_MIN)}")
    print(f"  Measurements  : {n_meas}")
    print()

    if not slots:
        print("  ⚠ Daylight window too short — no measurements scheduled.")
    else:
        print("  Scheduled measurement times:")
        print("  ─" * 24)
        for i, (hh, mm) in enumerate(slots, 1):
            print(f"    [{i:2d}]  {hh:02d}:{mm:02d}")
    print()

    # ── Show today's next upcoming measurement ─────────────────────────────────
    if target == date.today() and slots:
        now_min = datetime.now().hour * 60 + datetime.now().minute
        upcoming = [(hh, mm) for hh, mm in slots if hh * 60 + mm > now_min]
        if upcoming:
            hh, mm = upcoming[0]
            delta_min = hh * 60 + mm - now_min
            print(f"  ⏱ Next measurement: {hh:02d}:{mm:02d}  (in {delta_min // 60}h {delta_min % 60}m)")
        else:
            print("  ✓ All measurements for today are done.")
        print()


if __name__ == "__main__":
    main()
