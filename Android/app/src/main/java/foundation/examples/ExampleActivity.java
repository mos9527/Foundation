package foundation.examples;

import org.libsdl.app.SDLActivity;

public class ExampleActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        String libName = getIntent().getStringExtra("android.app.lib_name");
        if (libName == null || libName.isEmpty()) {
            libName = "Example_Triangle";
        }
        return new String[] {
            "SDL3",
            libName
        };
    }
}
