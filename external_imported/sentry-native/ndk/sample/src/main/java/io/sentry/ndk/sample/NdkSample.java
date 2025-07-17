package io.sentry.ndk.sample;

public class NdkSample {
  static {
    System.loadLibrary("ndk-sample");
  }

  public static native void crash();

  public static native void message();

  public static native void transaction();
}
