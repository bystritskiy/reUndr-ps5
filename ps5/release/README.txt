reUndr: an unofficial native PS5 port of Need for Speed: Underground (Xbox)

This package holds no game files. You need your own copy of the game: the NTSC-U
Xbox release, as a disc image (.iso) you made from your disc or as unpacked files.
A clean dump and a region or media patched one both work; other releases may not.
It is not affiliated with or endorsed by Electronic Arts or Sony.

INSTALL

1. Copy the PPSA99025 folder to /data/homebrew/PPSA99025 and let ShadowMountPlus
   register it, or install it from your store.
2. Copy your disc image (.iso) to /data/nfsu1/ on the console, or to the top of a
   USB drive or its nfsu1 folder. A trimmed image and a full dump both work.
3. Start reUndr. The first start unpacks the image to /data/nfsu1/game, about
   1.6 GB, with a progress bar, and then the game starts. After that you can delete
   the .iso.

Instead of an image you can copy unpacked files to /data/nfsu1/game, or to
nfsu1/game on a USB drive (/mnt/usb0/nfsu1/game to /mnt/usb7/nfsu1/game). The
folder must hold default.xbe next to CARS, TRACKS, FRONTEND, GLOBAL, SOUND, MOVIES,
LANGUAGES and MEMCARD.

If no disc is found, the app says so on screen.

HD LOGOS AND ICONS (OPTIONAL)

The package already has HD fonts, PlayStation button prompts and PlayStation
wording. Logos, badges and arrows come from the disc and stay as they are unless
you make the HD pack yourself on a computer, from the source repository:

    python3 -m pip install -r tools/requirements.txt
    python3 tools/prepare-disc.py --game <disc.iso> --out <folder>

It also needs potrace and cairo (brew install potrace cairo).
Copy <folder>/nfsu1/hd to /data/nfsu1/hd, or to nfsu1/hd on the USB drive that
holds the game.

CONTROLS

Cross/Circle/Square/Triangle = A/B/X/Y, R1/L1 = Black/White, L2/R2 = triggers,
Options = Start, touchpad press = Back, L3/R3 = stick presses. For a second player,
sign in a second local user (a guest will do); assist mode does not add a player.

SOUND AND DETAIL

reUndr plays the game's 5.1 mix on a surround system and falls back to stereo by
itself. Options > Port Settings has All Speakers (music and menus on every
speaker, on by default), Speaker Test (one speaker at a time; set it back to
Off) and LOD Distance (8x by default).

Settings, saves and logs are in /data/nfsu1/PPSA99025/. Licences are in licenses/.
