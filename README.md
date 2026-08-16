HOST: meant to run on a docker container. Wil scan what music you are listening to on spotify. downloads the album art + metadata. 
      if no music is running -> some placeholder will be uploaded
GUEST: checks every 5 seconds if HOST uploaded something new. displays currently available data from host on display

http://127.0.0.1:5000/api/info
http://127.0.0.1:5000/api/art
http://127.0.0.1:5000/api/art/cover.png