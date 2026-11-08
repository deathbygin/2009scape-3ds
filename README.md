# 2009scape 3DS

A native client for [2009scape](https://2009scape.org) on the New Nintendo 3DS, written in C with libctru and citro3d. It plays revision 530, the version 2009scape is built on.

This is a work in progress, shared so people can follow along. **It is not ready to play yet.** There are no releases so far, and it has only been tested against a private local server.

## Status

**Working on real hardware:** the 3D world, walking, objects and doors, NPCs and other players with animation, chat heads and dialogue, hit splats, ground items, and logging in and out. These interface tabs work too: inventory, worn equipment, skills and skill guides, quests and the quest journal, combat, prayer, emotes, music, all three spellbooks, options, friends, ignore and chat. So do the bank, shops and the on-screen keyboard.

**Built, still being tested:** trading, clan chat, the Summoning tab with familiar storage and pouch making, make-X screens such as fletching and smelting, and random events like the Quiz Master and the pillory.

**Known problem:** on real hardware the graphics can occasionally break up during play, and closing the app afterwards can leave the HOME Menu garbled until the console is restarted. This is being investigated now.

**Still to come:** loading the map in the background (there is a pause at the edge of each area), textures, sound and music, minimap icons, and remaining features such as duels, bank tabs and the Achievement Diary.

## Official servers

The client will not be pointed at the official 2009scape servers until it is complete, and not before talking with the 2009scape team.

## Requirements

- A New Nintendo 3DS, New 3DS XL or New 2DS XL with custom firmware. The original 3DS family does not have enough memory.
- Your own copy of the game data (see Setting up). None of the game's files or artwork are included here.

## Building

You need [devkitPro](https://devkitpro.org/wiki/Getting_Started) with devkitARM, libctru and citro3d, plus `makerom`, `bannertool` and Python 3.

```sh
cd app
make                # builds rs3ds_client.3dsx
bash cia/build.sh   # builds rs3ds_client.cia, for installing as a title
```

The `.cia` version needs a full power off and on after installing. Otherwise it can start without the extra memory it needs.

## Setting up

The client reads the game data from the SD card:

```
sdmc:/3ds/rs3ds/cache/       the revision 530 cache (main_file_cache.dat2 and .idx files)
sdmc:/3ds/rs3ds/xteas.json   map keys
sdmc:/3ds/rs3ds/server.txt   optional: server address, then port and username
```

The cache and `xteas.json` come with the [2009scape server](https://gitlab.com/2009scape/2009scape) (`Server/data/cache` and `Server/data/configs/xteas.json`). A `server.txt` might read `192.168.1.10 43595 myname`.

## Controls

The stylus does most things. The game is on one screen and the interface is on the other, and Select swaps them. Start closes the app.

## Disclaimer

This is an unofficial fan project, not made by or connected with Jagex or the 2009scape team.
