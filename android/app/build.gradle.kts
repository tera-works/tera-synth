plugins {
    id("com.android.application")
}

android {
    namespace = "com.hsynth.app"
    compileSdk = 36
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "com.hsynth.app"
        minSdk = 28
        targetSdk = 36
        versionCode = 3
        versionName = "0.3.0"

        ndk {
            // arm64 for Galaxy S23; x86_64 for the emulator
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Signed with the debug key so the APK can be side-loaded directly.
            // Replace with a real keystore for distribution.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    buildFeatures {
        prefab = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    androidResources {
        // keep .sfz / .wav / .json assets uncompressed-friendly and never ignore them
        noCompress += listOf("wav")
    }
}

dependencies {
    // Oboe (Apache-2.0): low-latency audio output via AAudio / OpenSL ES
    implementation("com.google.oboe:oboe:1.10.0")
    // ONNX Runtime (MIT): Spleeter 4-stem separation on the Edge TPU via NNAPI (CPU fallback)
    implementation("com.microsoft.onnxruntime:onnxruntime-android:1.20.0")
}
