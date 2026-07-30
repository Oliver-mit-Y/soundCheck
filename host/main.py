import os
from pathlib import Path

import spotipy
import yaml
from spotipy.oauth2 import SpotifyOAuth


def load_config():
    config_path = Path(__file__).resolve().with_name("conf.yml")
    if not config_path.exists():
        return {}

    with config_path.open("r", encoding="utf-8") as handle:
        return yaml.safe_load(handle) or {}


def spotipy_setup():
    config = load_config()
    print(config)

    client_id = config.get("SPOTIPY_CLIENT_ID") or os.getenv("SPOTIPY_CLIENT_ID")
    client_secret = config.get("SPOTIPY_CLIENT_SECRET") or os.getenv("SPOTIPY_CLIENT_SECRET")
    redirect_uri = config.get("SPOTIPY_REDIRECT_URI") or os.getenv("SPOTIPY_REDIRECT_URI")

    scope = "user-read-playback-state user-read-currently-playing"

    auth_manager = SpotifyOAuth(
        client_id=client_id,
        client_secret=client_secret,
        redirect_uri=redirect_uri,
        scope=scope,
    )
    sp = spotipy.Spotify(auth_manager=auth_manager)
    return sp

def main():
    sp = spotipy_setup()

    print(sp.current_playback())

if __name__ == "__main__":
    main()
    