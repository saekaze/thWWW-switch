<img width="256" height="256" alt="image" src="https://github.com/user-attachments/assets/c29c369a-ef82-4d4e-8b60-7ccb3ddfa7b9" />

- **Safer saves.** `Data.ini` (progress, unlocks, spell history, scores) is rewritten whenever a spell card starts. It used to be overwritten in place, so a crash or power loss at that moment could leave it empty. It is now written to `Data.ini.tmp` and swapped in, and a leftover complete `.tmp` is picked up on the next launch.

Includes everything from update 2 (stage 5 fix, Key Config, name entry, faster dense patterns; overclock recommended for Lunatic).

Replace thwww.nro in /switch/thwww/. Your save/ folder is kept.
