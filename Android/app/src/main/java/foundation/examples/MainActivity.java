package foundation.examples;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Create layout programmatically to avoid needing XML layouts
        ScrollView scrollView = new ScrollView(this);
        scrollView.setBackgroundColor(Color.BLACK);
        
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(64, 128, 64, 128);
        layout.setGravity(Gravity.CENTER_HORIZONTAL);
        scrollView.addView(layout);

        // Title
        TextView title = new TextView(this);
        title.setText("Foundation Examples");
        title.setTextColor(Color.WHITE);
        title.setTextSize(24f);
        title.setGravity(Gravity.CENTER);
        title.setPadding(0, 0, 0, 64);
        layout.addView(title);

        // Buttons for each example currently built
        String[] examples = {
            "Example_Triangle",
            "Example_HeadlessTriangle",
            "Example_SDF2D",
            "Example_MandelbrotCompute",
            "Example_BindlessSimple",
            "Example_DebugText",
            "Example_JobGraph",
            "Example_ImGui",
            "Example_MipGeneration",
            "Example_MeshShaderHello",
            "Example_MeshShaderHierarchicalLOD",
            "Example_CIEChromacity",
            "Example_GPUScene",
            "Example_GPUSceneStreaming",
            "Example_GPUSceneDeform",
            "Example_HeadlessPathTracer"
        };

        for (final String example : examples) {
            Button btn = new Button(this);
            btn.setText(example.replace("Example_", ""));
            btn.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    Intent intent = new Intent(MainActivity.this, ExampleActivity.class);
                    intent.putExtra("android.app.lib_name", example);
                    startActivity(intent);
                }
            });
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 
                LinearLayout.LayoutParams.WRAP_CONTENT
            );
            params.setMargins(0, 0, 0, 32);
            layout.addView(btn, params);
        }

        setContentView(scrollView);
    }
}
