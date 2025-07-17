# Android NDK support for sentry-native

| Package                       | Maven Central                                                                                                                                                                            | Minimum Android API Level | Supported ABIs                              |
|-------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------|---------------------------------------------|
| `io.sentry:sentry-native-ndk` | [![Maven Central](https://maven-badges.herokuapp.com/maven-central/io.sentry/sentry-native-ndk/badge.svg)](https://maven-badges.herokuapp.com/maven-central/io.sentry/sentry-native-ndk) | 19                        | "x86", "armeabi-v7a", "x86_64", "arm64-v8a" |

## Resources

- [SDK Documentation](https://docs.sentry.io/platforms/native/)
- [Discord](https://discord.gg/ez5KZN7) server for project discussions
- Follow [@getsentry](https://twitter.com/getsentry) on Twitter for updates

## About

The subproject aims to automatically bundle pre-built `sentry-native` binaries together with a Java JNI layer into an Android friendly `.aar` package.

The `.aar` package also provides [prefab](https://developer.android.com/build/native-dependencies?buildsystem=cmake) support, giving you the possibility to consume the native `sentry.h` APIs from your native app code.

If you're using the [Sentry Android SDK](https://docs.sentry.io/platforms/android/), this package is included by default already.

Besides the main package in `ndk/lib`, a simple Android app for testing purposes is provided in the `ndk/sample` folder.

## Building and Installation

The `ndk` project uses the Gradle build system in combination with CMake. You can either use a suitable IDE (e.g. Android Studio) or the command line to build it.

## Testing and consuming a local package version

1. Set a custom `versionName` in the `ndk/gradle.properties` file
2. Publish the package locally

   ```shell
   cd ndk
   ./gradlew :sentry-native-ndk:publishToMavenLocal
   ```

3. Consume the build in your app

   ```
   // usually settings.gradle
   allprojects {
     repositories {
       mavenLocal()
     }
   }

   // usually app/build.gradle
   android {
       buildFeatures {
           prefab = true
       }
   }

   dependencies {
        implementation("io.sentry:sentry-native-ndk:<version>")
   }
   ```

4. Link the pre-built packages with your native code

   ```cmake
   # usually app/CMakeLists.txt

   find_package(sentry-native-ndk REQUIRED CONFIG)

   target_link_libraries(<app> PRIVATE
       ${LOG_LIB}
       sentry-native-ndk::sentry-android
       sentry-native-ndk::sentry
   )
   ```

## Development

Please see the [contribution guide](../CONTRIBUTING.md).
