import requests
import time
import datetime
import subprocess
import argparse
from zoneinfo import ZoneInfo

TEAM_ID = 139  # Tampa Bay Rays
CHECK_INTERVAL = 60  # seconds
PREGAME_OFF_HOURS = 3  # turn USB off this many hours before game start

# Anchor all scheduling logic to the Rays' local time, so behavior is
# independent of the host's system timezone (e.g. the Pi runs in BST/UTC).
TEAM_TZ = ZoneInfo("America/New_York")


def now_local():
    return datetime.datetime.now(TEAM_TZ)


def today_local():
    return now_local().date()


def log(message):
    ts = now_local().strftime("%Y-%m-%d %H:%M:%S %Z")
    print(f"[{ts}] {message}", flush=True)

HUB = "1-1"
PORT = "2"

API_URL = "https://statsapi.mlb.com/api/v1/schedule"

last_game_id = None
celebrated_today = False
pregame_off_done = False
last_reset_date = None


def usb_on():
    subprocess.run(["sudo", "uhubctl", "-l", HUB, "-p", PORT, "-a", "on"])


def usb_off():
    subprocess.run(["sudo", "uhubctl", "-l", HUB, "-p", PORT, "-a", "off"])


def blink_usb(duration=10, interval=0.5):
    end = time.time() + duration
    state = False
    while time.time() < end:
        if state:
            usb_on()
        else:
            usb_off()
        state = not state
        time.sleep(interval)
    usb_on()  # leave ON


def get_today_game():
    today = today_local().isoformat()
    params = {
        "sportId": 1,
        "teamId": TEAM_ID,
        "date": today
    }

    r = requests.get(API_URL, params=params)
    data = r.json()

    if not data.get("dates"):
        return None

    games = data["dates"][0]["games"]
    if not games:
        return None

    return games[0]


def get_previous_game():
    today = today_local()
    start = (today - datetime.timedelta(days=7)).isoformat()
    end = today.isoformat()

    params = {
        "sportId": 1,
        "teamId": TEAM_ID,
        "startDate": start,
        "endDate": end,
    }

    r = requests.get(API_URL, params=params)
    data = r.json()

    if not data.get("dates"):
        return None

    previous_game = None
    for date_entry in data["dates"]:
        for game in date_entry["games"]:
            if game["status"].get("abstractGameState") == "Final":
                previous_game = game

    return previous_game


def rays_won(game):
    teams = game["teams"]
    rays = teams["home"] if teams["home"]["team"]["id"] == TEAM_ID else teams["away"]

    return rays["isWinner"]


def game_start_dt(game):
    """Return the game's scheduled start as a tz-aware datetime, or None."""
    if not game:
        return None
    game_dt_str = game.get("gameDate")
    if not game_dt_str:
        return None
    try:
        return datetime.datetime.fromisoformat(game_dt_str.replace("Z", "+00:00"))
    except ValueError:
        return None


def reset_if_new_day():
    global celebrated_today, pregame_off_done, last_reset_date
    today = today_local()
    if last_reset_date != today:
        celebrated_today = False
        pregame_off_done = False
        last_reset_date = today


def startup_celebration():
    """Blink once at startup if the most recent Final was a Rays win within
    the last ~24 hours. Otherwise just set steady USB state. Also marks
    today's game as celebrated to avoid a double-blink from the main loop."""
    global celebrated_today, last_game_id

    previous = get_previous_game()
    if not previous:
        log("Startup: no recent Final found.")
        return

    prev_status = previous["status"]["detailedState"]
    won = rays_won(previous)

    # gameDate is ISO 8601 UTC (e.g. "2026-05-07T23:10:00Z"). Use it to
    # decide both "recent" (<=24h since game start) and "is today's game".
    recent = False
    is_today = False
    game_dt_str = previous.get("gameDate")
    if game_dt_str:
        try:
            game_dt = datetime.datetime.fromisoformat(
                game_dt_str.replace("Z", "+00:00")
            )
            age = datetime.datetime.now(datetime.timezone.utc) - game_dt
            recent = age <= datetime.timedelta(hours=24)
            is_today = game_dt.astimezone(TEAM_TZ).date() == today_local()
        except ValueError:
            pass

    log(
        f"Startup: previous game {prev_status}, won={won}, "
        f"recent={recent}, is_today={is_today}"
    )

    if won and recent:
        log("Recent Rays win — celebrating at startup.")
        blink_usb()
    elif won:
        log("Older Rays win — USB ON without celebrating.")
        usb_on()
    else:
        log("Rays lost the previous game — USB OFF.")
        usb_off()

    # If we just handled today's game, suppress the in-loop celebration so
    # we don't blink twice for the same game.
    if is_today:
        celebrated_today = True
        last_game_id = previous["gamePk"]


def main():
    global last_game_id, celebrated_today, pregame_off_done

    usb_off()
    startup_celebration()

    while True:
        try:
            reset_if_new_day()
            now = now_local()

            game = get_today_game()
            start = game_start_dt(game)
            cutoff = (
                start - datetime.timedelta(hours=PREGAME_OFF_HOURS)
                if start else None
            )

            # Until the pregame cutoff (or all day if no game scheduled),
            # USB state reflects the most recent finished game.
            if cutoff is None or now < cutoff:
                if cutoff is not None:
                    log(
                        f"Pregame cutoff at {cutoff.astimezone(TEAM_TZ)} "
                        f"(in {cutoff - now})."
                    )
                previous = get_previous_game()
                if previous:
                    prev_status = previous["status"]["detailedState"]
                    log(f"Previous game: {prev_status}")
                    if rays_won(previous):
                        log("Rays won the previous game. USB ON.")
                        usb_on()
                    else:
                        log("Rays lost the previous game. USB OFF.")
                        usb_off()
                else:
                    log("No recent finished game found.")
                time.sleep(CHECK_INTERVAL)
                continue

            # Within PREGAME_OFF_HOURS of first pitch: turn USB off once,
            # then watch for the Final to celebrate.
            if not pregame_off_done:
                log(
                    f"Within {PREGAME_OFF_HOURS}h of game time. USB OFF."
                )
                usb_off()
                pregame_off_done = True

            log(f"Rays game {celebrated_today}")

            if game:
                game_id = game["gamePk"]
                status = game["status"]["detailedState"]
                log(status)

                if status == "Final" and not celebrated_today:
                    if rays_won(game):
                        log("Rays won! Celebrating...")
                        blink_usb()
                    else:
                        log("Rays lost.")

                    celebrated_today = True
                    last_game_id = game_id

            time.sleep(CHECK_INTERVAL)

        except Exception as e:
            log(f"Error: {e}")
            time.sleep(30)


if __name__ == "__main__":

    parser = argparse.ArgumentParser()
    parser.add_argument("--test-usb", action="store_true", help="Test USB blink")
    parser.add_argument("--usb-on", action="store_true", help="Force USB ON")
    parser.add_argument("--usb-off", action="store_true", help="Force USB OFF")

    args = parser.parse_args()

    if args.test_usb:
        log("Testing USB blink...")
        blink_usb()
    elif args.usb_on:
        log("Turning USB ON")
        usb_on()
    elif args.usb_off:
        log("Turning USB OFF")
        usb_off()
    else:
        main()
