// ===[ GCN boot self-verifying logger ]===
// Writes a FRESH numbered file per boot (HWLOG<n>.TXT) with immediate
// flush (this card's FAT mangles appended files). Each entry is checked
// against the expected boot timeline so the card can be read afterwards
// and compared line-by-line to what SHOULD have happened.
#include <gccore.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define GCN_BOOTLOG_MAX 256
static char g_bootlogPath[128];
static FILE* g_bootlogFile = NULL;
static int g_bootlogSeq = 0;
static int g_bootlogLines = 0;

// The expected boot timeline (real DELTARUNE ch1 + Wii port experience).
typedef struct { const char* tag; const char* expect; } ExpectStep;
static const ExpectStep kExpected[] = {
    { "MOUNT",     "SD mounted (fatInitDefault ok)" },
    { "DATAWIN",   "chapter1_windows/data.win opened" },
    { "PARSE",     "all 31 chunks parsed (TXTR 30/31)" },
    { "VM",        "VM init: 11158 globals" },
    { "ROOM0",     "ROOM_INITIALIZE loaded, 7 instances" },
    { "CONTACT",   "PLACE_CONTACT loaded, 8 instances" },
    { "FIRSTDRAW", "first frame blitted (cmds>0)" },
    { "FONTPAGE",  "font page decoded (page <= 2)" },
    { "TEXT",      "text pixels on screen" },
};
#define EXPECT_COUNT (int)(sizeof(kExpected) / sizeof(kExpected[0]))
static signed char kSeen[EXPECT_COUNT]; // 0=pending 1=ok -1=missing

int GCN_bootlog_expectIndex(const char* tag) {
    for (int i = 0; i < EXPECT_COUNT; ++i) {
        if (strcmp(kExpected[i].tag, tag) == 0) return i;
    }
    return -1;
}

void GCN_bootlog_mark(const char* tag, int ok) {
    int i = GCN_bootlog_expectIndex(tag);
    if (i >= 0) kSeen[i] = (signed char)(ok ? 1 : -1);
}

static void bootlog_scanSeq(void) {
    DIR* dir = opendir("/apps/DELTARUNEGC");
    g_bootlogSeq = 0;
    if (dir != NULL) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL) {
            int n = 0;
            if (sscanf(ent->d_name, "HWLOG%d.TXT", &n) == 1 && n >= g_bootlogSeq) {
                g_bootlogSeq = n + 1;
            }
        }
        closedir(dir);
    }
}

void GCN_bootlog_open(void) {
    bootlog_scanSeq();
    for (int attempt = 0; attempt < 8; ++attempt) {
        snprintf(g_bootlogPath, sizeof(g_bootlogPath),
            "/apps/DELTARUNEGC/HWLOG%d.TXT", g_bootlogSeq + attempt);
        int fd = open(g_bootlogPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            close(fd);
            g_bootlogFile = fopen(g_bootlogPath, "w");
            if (g_bootlogFile != NULL) {
                g_bootlogSeq += attempt;
                break;
            }
        }
        g_bootlogFile = NULL;
    }
    if (g_bootlogFile != NULL) {
        setvbuf(g_bootlogFile, NULL, _IONBF, 0);
        fprintf(g_bootlogFile, "=== SPAMTON LAUNCHER HWLOG %d ===\n", g_bootlogSeq);
        fprintf(g_bootlogFile, "expected timeline:\n");
        for (int i = 0; i < EXPECT_COUNT; ++i) {
            fprintf(g_bootlogFile, "  [%s] %s\n", kExpected[i].tag, kExpected[i].expect);
        }
        fprintf(g_bootlogFile, "---\n");
        fflush(g_bootlogFile);
        fclose(g_bootlogFile);
        // reopen in append for the run
        g_bootlogFile = fopen(g_bootlogPath, "a");
        if (g_bootlogFile != NULL) setvbuf(g_bootlogFile, NULL, _IONBF, 0);
    }
}

void GCN_bootlog(const char* fmt, ...) {
    char line[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    char tagged[224];
    // tag extraction: log lines starting with [TAG] update the checklist
    const char* tagStart = (line[0] == '[') ? line + 1 : NULL;
    char tag[16] = {0};
    if (tagStart != NULL) {
        const char* close = strchr(tagStart, ']');
        if (close != NULL && (size_t)(close - tagStart) < sizeof(tag)) {
            memcpy(tag, tagStart, close - tagStart);
            int i = GCN_bootlog_expectIndex(tag);
            if (i >= 0 && kSeen[i] == 0) kSeen[i] = 1;
        }
    }
    (void)tagged;

    // console + serial (Dolphin) visibility:
    extern void SYS_Report(char const* const, ...);
    SYS_Report("%s\n", line);

    if (g_bootlogFile != NULL) {
        fprintf(g_bootlogFile, "%s\n", line);
        fflush(g_bootlogFile);
    }
    if (++g_bootlogLines > GCN_BOOTLOG_MAX && g_bootlogFile != NULL) {
        fclose(g_bootlogFile);
        g_bootlogFile = NULL;
    }
}

// Write the final comparison table (call from the loop every ~600 frames,
// and it also draws to the telemetry panel).
int GCN_bootlog_compare(char* out, int outSize) {
    int okCount = 0;
    int off = 0;
    for (int i = 0; i < EXPECT_COUNT; ++i) {
        const char* state =
            (kSeen[i] == 1) ? "OK" : (kSeen[i] < 0 ? "FAIL" : "PENDING");
        if (kSeen[i] == 1) okCount++;
        off += snprintf(out + off, outSize - off, "%s %s %s\n",
            state, kExpected[i].tag, kExpected[i].expect);
        if (off >= outSize) break;
    }
    return okCount;
}

int GCN_bootlog_haveFile(void) { return g_bootlogFile != NULL; }
const char* GCN_bootlog_path(void) { return g_bootlogPath; }