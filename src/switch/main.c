#include <loop.h>
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>
#include <GLES3/gl3.h>

/* For SDL_main */
#if defined(USE_SDL1)
#include <SDL/SDL_main.h>
#elif defined(USE_SDL2)
#include <SDL2/SDL_main.h>
#elif defined(USE_SDL3)
#include <SDL3/SDL_main.h>
#endif

int main(int argc, char* argv[]) {
    (void)argc;
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    fsdevMountSdmc();

    const char* gameRoot = "sdmc:/switch/thwww";
    const char* dataPath = "sdmc:/switch/thwww/data.win";
    const char* saveRoot = "sdmc:/switch/thwww/save";
    mkdir(gameRoot, 0777);
    mkdir(saveRoot, 0777);

    // Fail visibly instead of returning to hbmenu with no explanation when the
    // legally obtained PC data files have not been copied beside the NRO.
    if (access(dataPath, F_OK) != 0) {
        consoleInit(NULL);
        printf("Wonderful Waking World - thWWW-switch 1.0.1\n\n");
        printf("data.win was not found.\n\n");
        printf("Copy the contents of the official thWWW_1.0.1.zip to:\n");
        printf("  /switch/thwww/\n\n");
        printf("Required examples:\n");
        printf("  /switch/thwww/data.win\n");
        printf("  /switch/thwww/font/\n");
        printf("  /switch/thwww/music/\n");
        printf("  /switch/thwww/text/\n\n");
        printf("Press + to return.\n");
        consoleUpdate(NULL);

        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            svcSleepThread(16 * 1000 * 1000L);
        }
        consoleExit(NULL);
        fsdevUnmountAll();
        return 1;
    }

    CommandLineArgs args = {0};

    args.exitAtFrame = -1;
#ifdef ENABLE_VM_TRACING
    args.traceBytecodeAfterFrame = 0;
#endif
    args.speedMultiplier = 1.0;
    args.fastForwardSpeed = 0.0;
    args.osType = OS_WINDOWS;
    args.profilerFramesBetween = 0;
    args.loadType = DATAWINLOADTYPE_LOAD_IN_MEMORY_AHEAD_OF_TIME;
#if defined(ENABLE_MODERN_GL)
    args.renderer = MODERN_GL;
#else
    args.renderer = SOFTWARE;
#endif
    args.dataWinPath = dataPath;
    args.saveFolder = saveRoot;

    int ret = loop(args, argv[0]);
    freeCommandLineArgs(&args);
    return ret;
}
