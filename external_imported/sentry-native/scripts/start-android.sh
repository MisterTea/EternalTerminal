#!/usr/bin/env bash

# Adapted from:
# https://docs.microsoft.com/en-us/azure/devops/pipelines/ecosystems/android?view=azure-devops#test-on-the-android-emulator

echo "java version:"
java -version
echo "javac version"
javac -version
echo "PATH: $PATH"

ARCH=${ANDROID_ARCH:-"x86"}
API_LEVEL=${ANDROID_API:-"29"}
NDK_VERSION=${ANDROID_NDK:-"27.2.12479018"}
AVD_EMULATOR_NAME="sentry_android_${ARCH}"
IMAGE=${ANDROID_IMAGE:-"system-images;android-${API_LEVEL};google_apis;${ARCH}"}
NDK_PACKAGE=${NDK_IMAGE:-"ndk;${NDK_VERSION}"}

# Create an Android Virtual Device
echo "Create Test AVDs with..."
echo "ARCH = $ARCH"
echo "API_LEVEL = $API_LEVEL"
echo "AVD_EMULATOR_NAME = $AVD_EMULATOR_NAME"
echo "IMAGE = $IMAGE"
echo "NDK_PACKAGE = $NDK_PACKAGE"

echo "Install image and NDK..."
yes | $ANDROID_SDK_ROOT/cmdline-tools/latest/bin/sdkmanager --licenses
$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/sdkmanager --install "$IMAGE"
$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/sdkmanager --install "$NDK_PACKAGE"

echo "Create AVD..."
echo "no" | $ANDROID_HOME/cmdline-tools/latest/bin/avdmanager create avd -n $AVD_EMULATOR_NAME -k "$IMAGE" --force

echo "List available AVDs..."
$ANDROID_HOME/emulator/emulator -list-avds

echo "Check emulator acceleration..."
$ANDROID_HOME/emulator/emulator -accel-check

# Start emulator in background
echo "Starting emulator..."
nohup $ANDROID_HOME/emulator/emulator -avd $AVD_EMULATOR_NAME -no-snapshot > /dev/null 2>&1 &

# For debugging emulator start issues:
# echo "Starting emulator blocking..."
# $ANDROID_HOME/emulator/emulator -avd $AVD_EMULATOR_NAME -no-snapshot

echo "Wait for emulator availability..."
$ANDROID_HOME/platform-tools/adb wait-for-device shell 'ls'
echo "Verify emulator devices are running..."
$ANDROID_HOME/platform-tools/adb devices

echo "Emulator started."
