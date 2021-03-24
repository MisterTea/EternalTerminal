#!/usr/bin/env bash

# Adapted from:
# https://docs.microsoft.com/en-us/azure/devops/pipelines/ecosystems/android?view=azure-devops#test-on-the-android-emulator

ARCH=${ANDROID_ARCH:-"x86"}
API_LEVEL=${ANDROID_API:-"29"}
AVD_EMULATOR_NAME="sentry_android_${ARCH}"
IMAGE=${ANDROID_IMAGE:-"system-images;android-${API_LEVEL};google_apis;${ARCH}"}

# Create an Android Virtual Device
echo "no" | $ANDROID_HOME/tools/bin/avdmanager create avd -n $AVD_EMULATOR_NAME -k "$IMAGE" --force

$ANDROID_HOME/emulator/emulator -list-avds

echo "Starting emulator..."

# Start emulator in background
nohup $ANDROID_HOME/emulator/emulator -avd $AVD_EMULATOR_NAME -no-snapshot > /dev/null 2>&1 &
$ANDROID_HOME/platform-tools/adb wait-for-device shell 'while [[ -z $(getprop sys.boot_completed) ]]; do sleep 1; done; input keyevent 82'
$ANDROID_HOME/platform-tools/adb devices

echo "Emulator started."
