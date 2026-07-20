package foundation.examples;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.Color;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

public class MainActivity extends Activity {
    private static final int COLOR_BACKGROUND = Color.rgb(12, 14, 20);
    private static final int COLOR_CARD = Color.rgb(27, 32, 44);
    private static final int COLOR_CARD_STROKE = Color.rgb(47, 56, 75);
    private static final int COLOR_TEXT_PRIMARY = Color.rgb(245, 247, 250);
    private static final int COLOR_TEXT_SECONDARY = Color.rgb(173, 181, 196);

    private static final class Example {
        final String libraryName;
        final String title;
        final String description;

        Example(String libraryName, String title, String description) {
            this.libraryName = libraryName;
            this.title = title;
            this.description = description;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setStatusBarColor(COLOR_BACKGROUND);
        getWindow().setNavigationBarColor(COLOR_BACKGROUND);

        ScrollView scrollView = new ScrollView(this);
        scrollView.setBackgroundColor(COLOR_BACKGROUND);
        
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(dp(24), dp(48), dp(24), dp(34));
        layout.setGravity(Gravity.CENTER_HORIZONTAL);
        scrollView.addView(layout);
        addHeader(layout);

        // BEGIN GENERATED ANDROID EXAMPLES
        Example[] examples = {
            new Example("Example_BindlessSimple", "BindlessSimple", "Demonstrates bindless texture sampling with a pool of procedurally generated textures. The fullscreen shader cycles through many sampled images without rebinding descriptors."),
            new Example("Example_DebugText", "DebugText", "Example showing how to use CSDebugText to get the absolute minimum up and running - with something to display. You can copy-paste this into your own application to get started."),
            new Example("Example_SDF2D", "SDF2D", "Fullscreen 2D signed-distance-field shader demo. Ports Inigo Quilez-style distance functions into a minimal Foundation render pass."),
            new Example("Example_MipGeneration", "MipGeneration", "Generates a full mip chain for a texture on the GPU and displays the sampled result. Uses the shared cameraman image asset as a simple compute mip-generation test."),
            new Example("Example_AutodiffSimple", "AutodiffSimple", "GPU auto-differentiation demo: BC7-style block compression via gradient descent. Ports NVIDIA Falcor's TinyBC sample. A compute pass jointly optimizes BC interpolation weights and endpoints per 4x4 tile using reverse-mode autodiff (__bwd_diff) and Adam, then the decoded (compressed->decompressed) result is blitted to the screen."),
            new Example("Example_Triangle", "Triangle", "Hello-world graphics example that renders a single animated triangle. Useful as the smallest windowed Renderer + Vulkan setup path."),
            new Example("Example_MeshShaderHierarchicalLOD", "MeshShaderHierarchicalLOD", "Mesh shader hierarchical LOD demo using clustered bunny geometry. Streams DAG LOD data to the GPU and lets the shader select detail by threshold."),
            new Example("Example_ImGui", "ImGui", "Minimal Dear ImGui integration sample using the Foundation backend. Opens the standard ImGui demo window over a rendered frame."),
            new Example("Example_MeshShaderHello", "MeshShaderHello", "Smallest mesh-shader pipeline sample: task + mesh + fragment shaders draw one object. Useful for validating mesh shader support and dispatch wiring."),
            new Example("Example_MandelbrotCompute", "MandelbrotCompute", "Uses a compute shader to render the fractal directly into the backbuffer! Shows a compact compute pass with time-varying push constants."),
            new Example("Example_CIEChromacity", "CIEChromacity", "Visualizes CIE chromaticity data and common display gamut primaries. Includes an interactive view over spectral locus and XYZ matching curves.")
        };
        // END GENERATED ANDROID EXAMPLES

        for (final Example example : examples)
            addExampleCard(layout, example);

        setContentView(scrollView);
    }

    private void addHeader(LinearLayout layout) {
        TextView title = new TextView(this);
        title.setText("Foundation Examples");
        title.setTextColor(COLOR_TEXT_PRIMARY);
        title.setTextSize(30f);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.CENTER);
        layout.addView(title, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        ));

        TextView subtitle = new TextView(this);
        subtitle.setText("Select a Vulkan sample to run");
        subtitle.setTextColor(COLOR_TEXT_SECONDARY);
        subtitle.setTextSize(15f);
        subtitle.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams subtitleParams = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        );
        subtitleParams.setMargins(0, dp(8), 0, dp(32));
        layout.addView(subtitle, subtitleParams);
    }

    private void addExampleCard(LinearLayout layout, final Example example) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setGravity(Gravity.CENTER_VERTICAL);
        card.setPadding(dp(18), dp(16), dp(18), dp(16));
        card.setBackground(makeRoundedRect(COLOR_CARD, COLOR_CARD_STROKE, 18f));
        card.setClickable(true);
        card.setFocusable(true);
        card.setForeground(selectableForeground());
        card.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startExample(example);
            }
        });

        TextView title = new TextView(this);
        title.setText(example.title);
        title.setTextColor(COLOR_TEXT_PRIMARY);
        title.setTextSize(18f);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        card.addView(title, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        ));

        if (!example.description.isEmpty()) {
            TextView description = new TextView(this);
            description.setText(example.description);
            description.setTextColor(COLOR_TEXT_SECONDARY);
            description.setTextSize(14f);
            description.setLineSpacing(dp(2), 1.0f);
            LinearLayout.LayoutParams descriptionParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            );
            descriptionParams.setMargins(0, dp(8), 0, 0);
            card.addView(description, descriptionParams);
        }

        LinearLayout.LayoutParams cardParams = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        );
        cardParams.setMargins(0, 0, 0, dp(14));
        layout.addView(card, cardParams);
    }

    private void startExample(Example example) {
        Intent intent = new Intent(MainActivity.this, ExampleActivity.class);
        intent.putExtra("android.app.lib_name", example.libraryName);
        startActivity(intent);
    }

    private GradientDrawable makeRoundedRect(int fillColor, int strokeColor, float radiusDp) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(fillColor);
        drawable.setCornerRadius(dp(radiusDp));
        drawable.setStroke(dp(1), strokeColor);
        return drawable;
    }

    private android.graphics.drawable.Drawable selectableForeground() {
        TypedValue outValue = new TypedValue();
        getTheme().resolveAttribute(android.R.attr.selectableItemBackground, outValue, true);
        return getDrawable(outValue.resourceId);
    }

    private int dp(float value) {
        return (int)(value * getResources().getDisplayMetrics().density + 0.5f);
    }
}
