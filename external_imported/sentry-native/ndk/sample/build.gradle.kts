plugins {
    id("com.android.application")
    kotlin("android")
}

var sentryNativeSrc: String = "${project.projectDir}/../.."

android {
    compileSdk = 35
    namespace = "io.sentry.ndk.sample"
    buildFeatures.buildConfig = true

    defaultConfig {
        applicationId = "io.sentry.ndk.sample"
        minSdk = 21
        targetSdk = 34
        versionCode = 2
        versionName = project.version.toString()

        externalNativeBuild {
            cmake {
                arguments.add(0, "-DANDROID_STL=c++_shared")
                arguments.add(0, "-DSENTRY_NATIVE_SRC=$sentryNativeSrc")
            }
        }

        ndk {
            abiFilters.addAll(listOf("x86", "armeabi-v7a", "x86_64", "arm64-v8a"))
        }
    }

    externalNativeBuild {
        cmake {
            path("CMakeLists.txt")
        }
    }

    signingConfigs {
        getByName("debug") {
            storeFile = rootProject.file("debug.keystore")
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
            signingConfig = signingConfigs.getByName("debug") // to be able to run release mode
            isShrinkResources = true

            addManifestPlaceholders(
                mapOf(
                    "sentryDebug" to false,
                    "sentryEnvironment" to "release",
                ),
            )
        }
    }

    kotlinOptions {
        jvmTarget = JavaVersion.VERSION_1_8.toString()
    }
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }
    ndkVersion = "27.0.12077973"
}

dependencies {
    implementation(project(":sentry-native-ndk"))
}
