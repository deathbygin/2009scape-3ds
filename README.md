# 2009scape 3DS

This is a work in progress, shared so people can follow along. **It is not ready to play yet.** There are no releases so far, and it has only been tested against a private local server so far.

A native client for [2009scape](https://2009scape.org) on the New Nintendo 3DS, written in C with libctru and citro3d with AI assistance. Based on revision 530, the version 2009scape is built on.

Disclaimer: This project is created with AI assistance. Use it at your own risk and do not share this code anywhere that AI-generated code is not allowed.

## Status

**Working on real hardware:** the 3D world, walking, objects and doors, NPCs and other players with animation, chat heads and dialogue, hit splats, ground items, and logging in and out. These interface tabs work too: inventory, worn equipment, skills and skill guides, quests and the quest journal, combat, prayer, emotes, music, all three spellbooks, options, friends, ignore and chat. So do the bank, shops and the on-screen keyboard.

**Built:** trading, clan chat, the Summoning tab with familiar storage and pouch making, make-X screens such as fletching and smelting, and random events like the Quiz Master and the pillory.

**Known problem:** on real hardware the graphics can occasionally break up during play, and closing the app afterwards can leave the HOME Menu / Homebrew Menu garbled until the console is restarted.

**Still to come:** loading the map in the background (there is a pause at the edge of each area), textures, sound and music, minimap icons, and remaining features such as duels, bank tabs and the Achievement Diary.

## Requirements

- A New Nintendo 3DS, New 3DS XL or New 2DS XL with CFW. Does not work on O3DS.
- Your own copy of the game data (see Setting up). None of the game's files or artwork are included here.

## Setting up

The client reads the game data from the SD card:

```
sdmc:/3ds/rs3ds/cache/       the revision 530 cache (main_file_cache.dat2 and .idx files)
sdmc:/3ds/rs3ds/xteas.json   map keys
sdmc:/3ds/rs3ds/server.txt   optional: server address, then port and username
```

The cache and `xteas.json` come with the [2009scape server](https://gitlab.com/2009scape/2009scape) (`Server/data/cache` and `Server/data/configs/xteas.json`). A `server.txt` might read `192.168.1.10 43595 myname`.

## Controls

The stylus controls the bottom screen. A cursor controls the top screen. The game is on one screen and the interface is on the other, and select swaps them, allowing either control scheme to be used for either screen at anytime. 

## Disclaimer

This is an unofficial fan project, not made by or connected with Jagex or the 2009scape team.
