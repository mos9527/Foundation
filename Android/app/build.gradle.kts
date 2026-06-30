plugins {
    id("com.android.application")
}

android {
    namespace = "foundation.examples"
    compileSdk = 34

    defaultConfig {
        applicationId = "foundation.examples"
        minSdk = 33
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"

        externalNativeBuild {
            cmake {
                arguments(
                    "-DANDROID_STL=c++_shared",
                    "-DFOUNDATION_WITH_EXAMPLES=ON",
                    "-DFOUNDATION_RHIVULKAN_VALIDATION_LAYER=OFF",
                    "-DANDROID_PLATFORM=android-33",
                    "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384",
                    "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-z,max-page-size=16384",
                    "-Wno-deprecated"
                )
                targets("Example_Triangle", "Example_SDF2D", "Example_MandelbrotCompute", "Example_GPUScene")
            }
        }
        
        ndk {
            abiFilters.add("arm64-v8a")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
            version = "3.26.0+"
        }
    }
    
    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
    
    // Disable asset compression for shader files so we can read them directly or quickly extract them
    androidResources {
        noCompress.add("spv")
    }
}
