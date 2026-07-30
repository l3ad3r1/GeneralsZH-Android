// GeneralsX @feature android-port 30/07/2026
package me.generalsx.malispike;

import org.libsdl.app.SDLActivity;

public class MaliSpikeActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL3",
            "dxvk_d3d9",
            "main"
        };
    }

    @Override
    protected String getMainFunction() {
        return "SDL_main";
    }
}
