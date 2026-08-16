import json
import os
from pathlib import Path
import time
import requests
import spotipy
import yaml
from PIL import Image, ImageDraw, ImageOps
from spotipy.oauth2 import CacheFileHandler, SpotifyOAuth


def load_config():
    config_path = Path(__file__).resolve().with_name("conf.yml")
    if not config_path.exists():
        return {}

    with config_path.open("r", encoding="utf-8") as handle:
        return yaml.safe_load(handle) or {}


def spotipy_setup():
    config = load_config()
    client_id = config.get("SPOTIPY_CLIENT_ID") or os.getenv("SPOTIPY_CLIENT_ID")
    client_secret = config.get("SPOTIPY_CLIENT_SECRET") or os.getenv("SPOTIPY_CLIENT_SECRET")
    redirect_uri = config.get("SPOTIPY_REDIRECT_URI") or os.getenv("SPOTIPY_REDIRECT_URI") or "http://127.0.0.1:8080/callback"

    scope = "user-read-playback-state user-read-currently-playing"

    cache_path = Path(__file__).resolve().parent / ".cache"
    cache_handler = CacheFileHandler(cache_path=str(cache_path))

    auth_manager = SpotifyOAuth(
        client_id=client_id,
        client_secret=client_secret,
        redirect_uri=redirect_uri,
        scope=scope,
        open_browser=True,
        cache_handler=cache_handler,
    )
    sp = spotipy.Spotify(auth_manager=auth_manager)
    return sp


def image_convert(img):
    input_path = Path(img)
    output_dir = input_path.parent / "art"
    output_dir.mkdir(parents=True, exist_ok=True)

    with Image.open(input_path) as img_obj:
        img_obj = ImageOps.exif_transpose(img_obj).convert("RGBA")
        img_obj = img_obj.resize((64, 64), Image.LANCZOS)

        mask = Image.new("L", (64, 64), 0)
        draw = ImageDraw.Draw(mask)
        draw.ellipse((0, 0, 63, 63), fill=255)

        hole_radius = 6
        draw.ellipse((32 - hole_radius, 32 - hole_radius, 32 + hole_radius, 32 + hole_radius), fill=0)

        frames = []
        frameint=48
        for angle in range(frameint):
            angle=int(360/frameint*angle)
            rotated = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
            rotated_img = img_obj.rotate(-angle, expand=False, fillcolor=(0, 0, 0, 0))
            rotated.paste(rotated_img, (0, 0), rotated_img)

            composite = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
            composite.paste(rotated, (0, 0), mask)

            final_img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
            final_img.paste(composite, (0, 0))

            outline = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
            outline_draw = ImageDraw.Draw(outline)
            outline_draw.ellipse((0, 0, 63, 63), outline=(255, 255, 255, 255), width=1)
            final_img = Image.alpha_composite(final_img, outline)

            center_hole = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
            draw_hole = ImageDraw.Draw(center_hole)
            draw_hole.ellipse((32 - hole_radius, 32 - hole_radius, 32 + hole_radius, 32 + hole_radius), fill=(0, 0, 0, 255))
            draw_hole.ellipse((32 - hole_radius, 32 - hole_radius, 32 + hole_radius, 32 + hole_radius), outline=(255, 255, 255, 255), width=1)
            final_img = Image.alpha_composite(final_img, center_hole)

            frames.append(final_img)

        output_path = output_dir / "cover.gif"
        frames[0].save(
            output_path,
            save_all=True,
            append_images=frames[1:],
            duration=50,
            loop=0
        )


def main():
    sp = spotipy_setup()
    last_img = ''
    while True:
        time.sleep(2)
        try:
            info = sp.current_playback()
            if info and info.get("is_playing"):
                info = {
                    "song": info["item"]["name"],
                    "artist": info["item"]["artists"][0]["name"],
                    "album": info["item"]["album"]["name"],
                    "year": info["item"]["album"]["release_date"].split("-")[0],
                    "img": info["item"]["album"]["images"][1]["url"],
                }
                if info['img'] != last_img:
                    with open("./out/cover.jpg", "wb") as f:
                        img = requests.get(info["img"])
                        f.write(img.content)
                        f.close()

                    image_convert("./out/cover.jpg")

                    with open("./out/info.json", "w", encoding="utf-8") as f:
                        json.dump(info, f, sort_keys=True, indent=4, ensure_ascii=False)
                        f.close()
                        print('ye')
                    last_img = info['img']
            else: 
                info = None
                with open("./out/info.json", "w", encoding="utf-8") as f:
                                    json.dump(info, f, sort_keys=True, indent=4, ensure_ascii=False)
                                    f.close()
                                    

                
            print(info)
        except Exception:
            info = None
            with open("./out/info.json", "w", encoding="utf-8") as f:
                                json.dump(info, f, sort_keys=True, indent=4, ensure_ascii=False)
                                f.close()
            print("!!! NO CONNECTION or fail!!!")
    
if __name__ == "__main__":
    main()


'''
    {'device': {'id': '97e0501c5a3e0556093ac14d909e06a109c2a4dc', 'is_active': True, 'is_private_session': False, 'is_restricted': False, 'name': 'THINKPAD', 'supports_volume': True, 'type': 'Computer', 'volume_percent': 58},
      'shuffle_state': True, 
      'smart_shuffle': False, 
      'repeat_state': 'off', 
      'is_playing': True, 
      'timestamp': 1785431954550, 
      'context': None, 
      'progress_ms': 477867, 
      'item': {'album': {'album_type': 'album',
                          'artists': [{'external_urls': {'spotify': 'https://open.spotify.com/artist/4MVyzYMgTwdP7Z49wAZHx0'}, 
                                       'href': 'https://api.spotify.com/v1/artists/4MVyzYMgTwdP7Z49wAZHx0', 'id': '4MVyzYMgTwdP7Z49wAZHx0', 'name': 'Lynyrd Skynyrd', 'type': 'artist', 'uri': 'spotify:artist:4MVyzYMgTwdP7Z49wAZHx0'}], 
                                       'external_urls': {'spotify': 'https://open.spotify.com/album/6DExt1eX4lflLacVjHHbOs'}, 'href': 'https://api.spotify.com/v1/albums/6DExt1eX4lflLacVjHHbOs', 'id': '6DExt1eX4lflLacVjHHbOs', 
                            'images': [{'height': 640, 'url': 'https://i.scdn.co/image/ab67616d0000b273128450651c9f0442780d8eb8', 'width': 640}, {'height': 300, 'url': 'https://i.scdn.co/image/ab67616d00001e02128450651c9f0442780d8eb8', 'width': 300}, {'height': 64, 'url': 'https://i.scdn.co/image/ab67616d00004851128450651c9f0442780d8eb8', 'width': 64}], 
                            'name': "Pronounced' Leh-'Nerd 'Skin-'Nerd", 
                            'release_date': '1973-01-01', 'release_date_precision': 'day', 'total_tracks': 8, 'type': 'album', 'uri': 'spotify:album:6DExt1eX4lflLacVjHHbOs'},
                'artists': [{'external_urls': {'spotify': 'https://open.spotify.com/artist/4MVyzYMgTwdP7Z49wAZHx0'}, 'href': 'https://api.spotify.com/v1/artists/4MVyzYMgTwdP7Z49wAZHx0', 'id': '4MVyzYMgTwdP7Z49wAZHx0', 'name': 'Lynyrd Skynyrd', 'type': 'artist', 'uri': 'spotify:artist:4MVyzYMgTwdP7Z49wAZHx0'}], 'disc_number': 1, 'duration_ms': 547106, 'explicit': False, 'external_urls': {'spotify': 'https://open.spotify.com/track/5EWPGh7jbTNO2wakv8LjUI'}, 'href': 'https://api.spotify.com/v1/tracks/5EWPGh7jbTNO2wakv8LjUI', 'id': '5EWPGh7jbTNO2wakv8LjUI', 'is_local': False, 'name': 'Free Bird', 'preview_url': None, 'track_number': 8, 'type': 'track', 'uri': 'spotify:track:5EWPGh7jbTNO2wakv8LjUI'}, 'currently_playing_type': 'track', 'actions': {'disallows': {'resuming': True, 'toggling_repeat_context': True, 'toggling_repeat_track': True, 'toggling_shuffle': True}}}
    
    
    [{'external_urls': {'spotify': 'https://open.spotify.com/artist/4MVyzYMgTwdP7Z49wAZHx0'},
      'href': 'https://api.spotify.com/v1/artists/4MVyzYMgTwdP7Z49wAZHx0', 'id': '4MVyzYMgTwdP7Z49wAZHx0', 'name': 'Lynyrd Skynyrd', 'type': 'artist', 'uri': 'spotify:artist:4MVyzYMgTwdP7Z49wAZHx0'}]
    
    '''