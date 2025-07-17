plugins {
    id("com.android.library")
    kotlin("android")
    id("com.ydq.android.gradle.native-aar.export")
}

var sentryNativeSrc: String = "${project.projectDir}/../.."

android {
    compileSdk = 35
    namespace = "io.sentry.ndk"

    testBuildType = "debug"

    defaultConfig {
        minSdk = 21

        externalNativeBuild {
            cmake {
                arguments.add(0, "-DANDROID_STL=c++_static")
                arguments.add(0, "-DSENTRY_NATIVE_SRC=$sentryNativeSrc")
            }
        }

        ndk {
            abiFilters.addAll(listOf("x86", "armeabi-v7a", "x86_64", "arm64-v8a"))
        }

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    // we use the default NDK and CMake versions based on the AGP's version
    // https://developer.android.com/studio/projects/install-ndk#apply-specific-version
    externalNativeBuild {
        cmake {
            path("CMakeLists.txt")
        }
    }

    buildTypes {
        getByName("debug") {
            externalNativeBuild {
                cmake {
                    arguments.add(0, "-DENABLE_TESTS=ON")
                }
            }
        }
        getByName("release") {
            consumerProguardFiles("proguard-rules.pro")
        }
    }

    buildFeatures {
        prefabPublishing = true
        buildConfig = true
    }

    // creates
    // lib.aar/prefab/modules/sentry-android/libs/<arch>/<lib>.so
    // lib.aar/prefab/modules/sentry-android/include/sentry.h
    prefab {
        create("sentry-android") {}
        create("sentry") {
            headers = "../../include"
        }
    }

    // legacy pre-prefab support
    // https://github.com/howardpang/androidNativeBundle
    // creates
    // lib.aar/jni/<arch>/<lib>.so
    // lib.aar/jni/include/sentry.h
    nativeBundleExport {
        headerDir = "../../include"
    }

    kotlinOptions {
        jvmTarget = JavaVersion.VERSION_1_8.toString()
    }

    testOptions {
        animationsDisabled = true
        unitTests.apply {
            isReturnDefaultValues = true
            isIncludeAndroidResources = true
        }
    }

    lint {
        warningsAsErrors = true
        checkDependencies = true
        checkReleaseBuilds = true
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }
}

dependencies {
    compileOnly("org.jetbrains:annotations:23.0.0")

    testImplementation("androidx.test.ext:junit:1.2.1")

    androidTestImplementation("androidx.test:runner:1.6.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test:rules:1.6.1")
}

/*
 * Prefab doesn't support c++_static, so we need to change it to none.
 * This should be fine, as we don't expose any conflicting symbols.
 * Based on: https://github.com/bugsnag/bugsnag-android/blob/59460018551750dfcce4fd4e9f612eae7826559e/bugsnag-plugin-android-ndk/build.gradle.kts
 *
 * Copyright (c) 2012 Bugsnag

 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:

 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */
afterEvaluate {
    tasks.getByName("prefabReleasePackage") {
        doLast {
            project.fileTree("build/intermediates/prefab_package/") {
                include("**/abi.json")
            }.forEach { file ->
                file.writeText(file.readText().replace("c++_static", "none"))
            }
        }
    }
}
