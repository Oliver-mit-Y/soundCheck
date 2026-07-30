import json
import os

import spotipy
from spotipy.oauth2 import SpotifyOAuth


def spotipy_setup():
    # 1. Fetch credentials from your shell environment
    client_id = os.getenv("SPOTIPY_CLIENT_ID")
    client_secret = os.getenv("SPOTIPY_CLIENT_SECRET")
    redirect_uri = os.getenv("SPOTIPY_REDIRECT_URI")  # e.g., "http://localhost:8080"

    # 2. Define the permissions (scopes) your script needs
    scope = "user-read-playback-state user-read-currently-playing"

    # 3. Initialize the OAuth manager and the Spotipy client
    auth_manager = SpotifyOAuth(
        client_id=client_id,
        client_secret=client_secret,
        redirect_uri=redirect_uri,
        scope=scope
    )
    sp = spotipy.Spotify(auth_manager=auth_manager)
    return sp

def main():
    sp = spotipy_setup()

    print(sp.current_playback())

if __name__ == "__main__":
    main()
    