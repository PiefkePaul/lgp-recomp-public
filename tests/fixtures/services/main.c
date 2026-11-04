// Homebrew test fixture "services" (own code, libnx/newlib only): libnx's own
// service clients against the runtime's HLE services (P3.4), started by the
// boot program (tests/boot). Each step prints "<step>: ok" or
// "<step>: FAILED 0x<result>"; the last line says whether all passed.
//
// Expects (tests/boot/CMakeLists.txt sets this up): a RomFS with
// /hello.txt = "hello from romfs\n" and /dir/nested.bin = bytes 0..255, an
// empty save storage, the account nickname "lgp", and a keyboard that
// answers with the game's initial text. Only meaningful as NSO: an NRO
// carries its RomFS in the file and reads it through sdmc.
#include <stdio.h>
#include <string.h>
#include <switch.h>

// Services are opened step by step below, so a missing one names itself.
void __appInit(void) {}
void __appExit(void) {}

static int g_failed;

static void say(const char* s) { svcOutputDebugString(s, strlen(s)); }

// No printf: newlib's _svfprintf_r dispatches its format switch through a
// jump table whose base register is set before the parsing loop, which the
// recompiler's jump-table recognition does not follow yet (see
// docs/knowledge/jump-table-patterns.md).
static void report(const char* step, Result rc) {
    char buf[96];
    size_t n = 0;
    for (const char* p = step; *p != '\0' && n < 64; ++p)
        buf[n++] = *p;
    const char* verdict = R_SUCCEEDED(rc) ? ": ok" : ": FAILED 0x";
    for (const char* p = verdict; *p != '\0'; ++p)
        buf[n++] = *p;
    if (R_FAILED(rc)) {
        for (int i = 7; i >= 0; --i)
            buf[n++] = "0123456789abcdef"[(rc >> (i * 4)) & 0xF];
        ++g_failed;
    }
    svcOutputDebugString(buf, n);
}

static void check(const char* step, int ok) { report(step, ok ? 0 : MAKERESULT(Module_Libnx, LibnxError_BadInput)); }

static void test_romfs(void) {
    Result rc = romfsMountSelf("romfs");
    report("romfs mount", rc);
    if (R_FAILED(rc))
        return;
    char text[64] = {0};
    FILE* f = fopen("romfs:/hello.txt", "rb");
    size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
    if (f)
        fclose(f);
    check("romfs read hello.txt", n == 17 && strcmp(text, "hello from romfs\n") == 0);
    unsigned char bytes[300];
    f = fopen("romfs:/dir/nested.bin", "rb");
    n = f ? fread(bytes, 1, sizeof(bytes), f) : 0;
    if (f)
        fclose(f);
    int same = n == 256;
    for (size_t i = 0; same && i < 256; ++i)
        same = bytes[i] == i;
    check("romfs read dir/nested.bin", same);
    check("romfs missing file", fopen("romfs:/missing.txt", "rb") == NULL);
    romfsUnmount("romfs");
}

static void test_save(AccountUid uid) {
    FsFileSystem fs;
    Result rc = fsOpen_SaveData(&fs, 0, uid);
    report("save open", rc);
    if (R_FAILED(rc))
        return;
    static const char kData[] = "lgp save data";
    rc = fsFsCreateFile(&fs, "/slot0.bin", sizeof(kData), 0);
    report("save create file", rc);
    FsFile file;
    rc = fsFsOpenFile(&fs, "/slot0.bin", FsOpenMode_Read | FsOpenMode_Write, &file);
    report("save open file", rc);
    if (R_SUCCEEDED(rc)) {
        report("save write", fsFileWrite(&file, 0, kData, sizeof(kData), FsWriteOption_Flush));
        char back[sizeof(kData)] = {0};
        u64 got = 0;
        rc = fsFileRead(&file, 0, back, sizeof(back), FsReadOption_None, &got);
        report("save read", rc);
        check("save read back", got == sizeof(kData) && memcmp(back, kData, sizeof(kData)) == 0);
        fsFileClose(&file);
    }
    report("save commit", fsFsCommit(&fs));
    fsFsClose(&fs);
}

static void test_keyboard(void) {
    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    report("swkbd create", rc);
    if (R_FAILED(rc))
        return;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetInitialText(&kbd, "Pikachu");
    char out[64] = {0};
    rc = swkbdShow(&kbd, out, sizeof(out));
    report("swkbd show", rc);
    check("swkbd text", strcmp(out, "Pikachu") == 0);
    swkbdClose(&kbd);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    report("sm", smInitialize());
    report("applet", appletInitialize());
    check("applet in focus", appletGetFocusState() == AppletFocusState_InFocus);

    report("set", setInitialize());
    u64 code = 0;
    report("set language", setGetSystemLanguage(&code));
    check("set language is de", memcmp(&code, "de", 3) == 0);

    report("time", timeInitialize());
    u64 now = 0;
    report("time current", timeGetCurrentTime(TimeType_UserSystemClock, &now));
    // The clock starts at the configured time and then runs: a second
    // boundary between the time service starting and this call (slow
    // browser runners) adds one, so allow up to a minute.
    check("time is the configured one", now >= 1700000000 && now < 1700000060);

    report("csrng", csrngInitialize());
    u8 random[16] = {0};
    report("csrng bytes", csrngGetRandomBytes(random, sizeof(random)));

    report("acc", accountInitialize(AccountServiceType_Application));
    AccountUid uid = {0};
    report("acc preselected user", accountGetPreselectedUser(&uid));
    check("acc user valid", accountUidIsValid(&uid));
    AccountProfile profile;
    Result rc = accountGetProfile(&profile, uid);
    report("acc profile", rc);
    if (R_SUCCEEDED(rc)) {
        AccountProfileBase base;
        report("acc profile get", accountProfileGet(&profile, NULL, &base));
        check("acc nickname", strcmp(base.nickname, "lgp") == 0);
        accountProfileClose(&profile);
    }

    report("fs", fsInitialize());
    test_romfs();
    test_save(uid);
    test_keyboard();

    say(g_failed == 0 ? "services: all checks passed" : "services: some checks FAILED");
    return g_failed;
}
