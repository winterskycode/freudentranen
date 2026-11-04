package dev.tok.totk;

/** Which game this app runs. One launcher serves both apps built from the runtime: tools/build_apk.sh stamps KEY with
    TOTK_VERSION (v143: Tears of the Kingdom 1.4.3, the default; botw160: Breath of the Wild 1.6.0, tools/build_botw_apk.sh).
    The rest follows from it: the data folder the game process reads (host/app.c TOTK_GAME_DIR), the RomFS size the
    startup screen checks, the title and version shown, and whether the Mods page exists (it merges into 1.4.3's files). */
final class Game {
    static final String KEY = "v143";
    static final boolean BOTW = KEY.startsWith("botw");
    static final String DATA = "/sdcard/tok/" + KEY;
    static final String TITLE = BOTW ? "Breath of the Wild" : "Tears of the Kingdom";
    static final String VERSION = BOTW ? "1.6.0" : "1.4.3";
    /** romfs.bin's size: the merged RomFS of the update over the base game (MacNativeGames scripts/extract_romfs.py) */
    static final long ROMFS_SIZE = BOTW ? 15131979952L : 17052625232L;
    static final boolean MODS = !BOTW;
    /** Tools > Saves: Saves.java knows this game's slots (Breath of the Wild's save has another layout) */
    static final boolean SAVES = !BOTW;
    /** Tools > Game files and the first-run setup: the player's own NSPs, which only Tears of the Kingdom can play from */
    static final boolean SETUP = !BOTW;

    private Game() {}
}
