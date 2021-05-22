# qtmultimedia-plugin-ffmpeg

Qt multimedia plugins implemented on top of ffmpeg

Multimedia plugins are looked up in alphabetical order, 
so the ffmpeg plugin may be not selected. 

## Features
- All ffmpeg formats can be played. 
- You can replace the ffmpeg library to support more formats.
- OpenGL rendering.

## Build
- Build and install. Just qmake it in QtCreator.
- Make sure the right ffmpeg library is installed somewhere it can be found, see fmpeg-plugin.pro for details.
- This plugin has been tested and build against [ffmpeg 4.1](https://ffmpeg.org).
- This plugin uses QtAudioOuput for audioplayback as a default backend, but if you have SDL2 somewhere installed it will be loaded and used instead.
- Try a Qt multimedia example and use files that are supported (only) by ffmpeg

## Install
- Install the build plugin in your Qt environment (e.g. C:\Qt\5.15.2\msvc2019_64\plugins).
- Or you can store it somewhere else, where you load extra plugins for your program.

## Limitations
This plugin supports basic playback of video and audio. It uses ffmpeg solily as decoder backend. 

It doesn't implement scaling, hue, sat and brightness. Also the playback rate cannot be influenced.
I didn't need that for my project. However, you can fork this project and create that yourself. 

## Thanks
Thanks to Wang Bin for creating the basis for this multimedia plugin. I used his framework. 
He has a great product [MDK](https://github.com/wang-bin/qtmultimedia-plugins-mdk) that is much better 
than this ffmpeg plugin, but MDK is not free for commercial use. 

Thanks to all persons who created programs and examples for using ffmpeg to stream video/audio.
Without it, I would have been lost. 

## License
This plugin library for Qt is licensed under LGPLv2.1. 

## See Also
- Checkout the zcVideoWidget that can be used to display video that is being played with the Qt MultiMedia framework


