import json
import os
import requests
import spotipy
from spotipy.oauth2 import SpotifyOAuth


def build_song_payload(track_data):
    if not track_data or not track_data.get("is_playing") or not track_data.get("item"):
        return {
            "is_playing": False,
            "song": None,
            "message": "No track is currently playing.",
        }

    item = track_data["item"]
    album = item.get("album", {})
    artists = [artist.get("name") for artist in item.get("artists", []) if artist.get("name")]

    return {
        "is_playing": True,
        "song": {
            "name": item.get("name"),
            "artists": artists,
            "album": album.get("name"),
            "link": item.get("external_urls", {}).get("spotify"),
            "id": item.get("id"),
            "uri": item.get("uri"),
            "release_date": album.get("release_date"),
            "album_art": album.get("images", [{}])[0].get("url") if album.get("images") else None,
            
        },
    }


def get_currently_playing_song():
    client_id = os.getenv("SPOTIPY_CLIENT_ID")
    client_secret = os.getenv("SPOTIPY_CLIENT_SECRET")
    redirect_uri = os.getenv("SPOTIPY_REDIRECT_URI", "http://localhost:8080/callback")

    if not client_id or not client_secret:
        raise RuntimeError(
            "Missing Spotify credentials. Export SPOTIPY_CLIENT_ID and SPOTIPY_CLIENT_SECRET in your shell."
        )

    auth_manager = SpotifyOAuth(
        client_id=client_id,
        client_secret=client_secret,
        redirect_uri=redirect_uri,
        scope="user-read-currently-playing",
        open_browser=False,
    )
    spotify_client = spotipy.Spotify(auth_manager=auth_manager)
    track_data = spotify_client.current_user_currently_playing_track()
    return build_song_payload(track_data)


if __name__ == "__main__":
    try:
        song = get_currently_playing_song()
        print(json.dumps(song, indent=2))
    except Exception as exc:
        print(json.dumps({"error": str(exc)}, indent=2))


    print(song['album_art'])
