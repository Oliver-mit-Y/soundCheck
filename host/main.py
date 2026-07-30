import json
import os

import spotipy
from spotipy.oauth2 import SpotifyOAuth




def main():
    sp = spotipy.Spotify()

    print(sp.current_playback())

if __name__ == "__main__":
    main()
    