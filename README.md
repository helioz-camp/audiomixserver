# audiomixserver
SDL2 based sound server mixing up samples

Uses

- SDL2
- libevent
- boost program options

Ubuntu

- apt-get install libevent-dev libboost-program-options-dev libsdl2-mixer-dev

- make

```
./audiomixserver --help
Mix audio:
  --help                              Display this help message
  --frequency arg (=48000)            Frequency in Hz
  --channels arg (=2)                 Channels
  --chunksize arg (=512)              Bytes sent to sound output each time, 
                                      divide by frequency to find duration
  --sample-files arg                  OGG, WAV or MP3 sample files
  --allocate_sdl_channels arg (=2048) Number of SDL channels to mix together
  --bind_address arg (=0.0.0.0)       Address to listen on
  --bind_port arg (=13231)            Port to listen on
```

- ./audiomixserver /usr/share/sounds/ubuntu/stereo/button-*

- visit localhost:13231/