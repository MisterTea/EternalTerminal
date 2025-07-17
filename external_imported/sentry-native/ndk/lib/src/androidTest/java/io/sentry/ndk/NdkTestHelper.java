package io.sentry.ndk;

public class NdkTestHelper {
  static {
    System.loadLibrary("sentry-android-test");
  }

  public static native void message();

  public static native void transaction();
}
