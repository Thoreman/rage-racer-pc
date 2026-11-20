#ifdef __ANDROID__

#include "android_bootstrap.h"

#include <SDL3/SDL.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Android has no $HOME/XDG layout. Root everything in the app's external
 * storage directory (/sdcard/Android/data/<package>/files), which adb can
 * write to without extra permissions:
 *
 *   files/rage-port.ini      optional config (used via RAGE_CONFIG)
 *   files/disc/<image>.cue   disc image with its BIN tracks, or a .chd
 *   files/rage-racer/        config/state written by the port
 */
static int HasSuffix(const char *name, const char *suffix) {
    size_t nameLength = strlen(name);
    size_t suffixLength = strlen(suffix);
    return nameLength >= suffixLength &&
           strcasecmp(name + nameLength - suffixLength, suffix) == 0;
}

static int FindDiscImage(const char *directory, char *out, size_t outSize) {
    static const char *const kSuffixes[] = {".cue", ".chd"};
    size_t pass;

    /* Prefer a CUE (full CD audio) over a CHD when both are present. */
    for (pass = 0; pass < sizeof(kSuffixes) / sizeof(kSuffixes[0]); pass++) {
        DIR *dir = opendir(directory);
        struct dirent *entry;
        if (dir == NULL) return 0;
        while ((entry = readdir(dir)) != NULL) {
            if (HasSuffix(entry->d_name, kSuffixes[pass])) {
                int written = snprintf(out, outSize, "%s/%s", directory,
                                       entry->d_name);
                closedir(dir);
                return written > 0 && (size_t)written < outSize;
            }
        }
        closedir(dir);
    }
    return 0;
}

/* The disc setup panel (DiscSetupActivity) writes the path of an image it
 * found elsewhere on the headset to files/disc-location.txt. */
static int RememberedDiscImage(const char *root, char *out, size_t outSize) {
    char note[4096];
    FILE *file;
    size_t length;

    snprintf(note, sizeof(note), "%s/disc-location.txt", root);
    file = fopen(note, "r");
    if (file == NULL) return 0;
    if (fgets(out, (int)outSize, file) == NULL) out[0] = '\0';
    fclose(file);
    length = strlen(out);
    while (length > 0 && (out[length - 1] == '\n' || out[length - 1] == '\r'))
        out[--length] = '\0';
    return length > 0 && access(out, R_OK) == 0;
}

void AndroidBootstrap(void) {
    const char *root = SDL_GetAndroidExternalStoragePath();
    char path[4096];
    struct stat info;

    if (root == NULL || root[0] == '\0') {
        SDL_Log("rage-port: no Android external storage path: %s",
                SDL_GetError());
        return;
    }
    mkdir(root, 0775);
    setenv("HOME", root, 0);
    setenv("XDG_CONFIG_HOME", root, 1);
    setenv("XDG_STATE_HOME", root, 1);
    if (chdir(root) != 0) {
        SDL_Log("rage-port: cannot enter %s", root);
    }

    snprintf(path, sizeof(path), "%s/rage-port.ini", root);
    if (getenv("RAGE_CONFIG") == NULL && stat(path, &info) == 0) {
        setenv("RAGE_CONFIG", path, 1);
    }

    snprintf(path, sizeof(path), "%s/disc", root);
    mkdir(path, 0775);
    if (getenv("RAGE_PORT_DISC_CUE") == NULL) {
        char image[4096];
        if (FindDiscImage(path, image, sizeof(image)) ||
            RememberedDiscImage(root, image, sizeof(image))) {
            setenv("RAGE_PORT_DISC_CUE", image, 1);
            SDL_Log("rage-port: using disc image %s", image);
        } else {
            SDL_Log("rage-port: no disc image found in %s", path);
        }
    }
    SDL_Log("rage-port: Android storage root %s", root);
}

#endif
