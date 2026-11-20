#ifndef RAGE_ANDROID_BOOTSTRAP_H
#define RAGE_ANDROID_BOOTSTRAP_H

#ifdef __ANDROID__
/* Points config, state and disc discovery at the app's external storage. */
void AndroidBootstrap(void);
#endif

#endif
