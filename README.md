HOST: meant to run on a docker container. Wil scan what music you are listening to on spotify. downloads the album art + metadata. 
      if no music is running -> some placeholder will be uploaded
GUEST: checks every 5 seconds if HOST uploaded something new. displays currently available data from host on display

http://127.0.0.1:5000/api/info
http://127.0.0.1:5000/api/art
http://127.0.0.1:5000/api/art/cover.png

sudo ./gif-and-text-api   --led-rows=64 --led-cols=64 --led-chain=1 --led-slowdown-gpio=2   --json-url=http://192.168.178.100:4567/api/info   --gif-url=http://192.168.178.100:4567/api/art/cover.gif   --keys=song,artist,almbum,year --font=../fonts/font.bdf   --text-bar-height=5 --scroll-speed=2 --gif-speed=900 --led-gpio-mapping=adafruit-hat-pwm --led-brightness=80 --poll-ms=3000