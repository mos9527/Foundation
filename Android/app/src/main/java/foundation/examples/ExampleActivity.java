package foundation.examples;

import android.os.Bundle;
import android.view.KeyEvent;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import org.libsdl.app.SDLActivity;

public class ExampleActivity extends SDLActivity {
    private boolean quitRequested;
    private OnBackInvokedCallback backCallback;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        backCallback = new OnBackInvokedCallback() {
            @Override
            public void onBackInvoked() {
                requestExampleQuit();
            }
        };
        getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
            OnBackInvokedDispatcher.PRIORITY_DEFAULT,
            backCallback
        );
    }

    @Override
    protected void onDestroy() {
        requestExampleQuit();
        if (backCallback != null) {
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backCallback);
            backCallback = null;
        }
        super.onDestroy();
    }

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

    @Override
    public void onBackPressed() {
        requestExampleQuit();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            if (event.getAction() == KeyEvent.ACTION_UP)
                requestExampleQuit();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    private void requestExampleQuit() {
        if (quitRequested)
            return;

        quitRequested = true;
        SDLActivity.nativeSendQuit();
    }
}
