package org.rageracer.quest;

import android.content.Intent;
import android.os.Bundle;

import org.libsdl.app.SDLActivity;

/** SDL is linked statically into libmain.so, so only that library is loaded. */
public class RageRacerActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        boolean needsDisc = !DiscLocator.hasDisc(this);
        super.onCreate(savedInstanceState);
        if (needsDisc) {
            /* No disc image: show the flat setup panel instead. It runs in
             * its own process, so this one can go before the game starts
             * without a disc; the panel launches the game afresh when done. */
            startActivity(new Intent(this, DiscSetupActivity.class)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            finish();
            System.exit(0);
        }
    }
}
