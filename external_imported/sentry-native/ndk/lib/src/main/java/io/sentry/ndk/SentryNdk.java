package io.sentry.ndk;

import org.jetbrains.annotations.ApiStatus;
import org.jetbrains.annotations.NotNull;

@ApiStatus.Internal
public final class SentryNdk {

  private static volatile boolean nativeLibrariesLoaded;

  private SentryNdk() {}

  private static native void initSentryNative(@NotNull final NdkOptions options);

  private static native void shutdown();

  /**
   * Init the NDK integration
   *
   * @param options the SentryAndroidOptions
   */
  public static void init(@NotNull final NdkOptions options) {
    loadNativeLibraries();
    initSentryNative(options);
  }

  /** Closes the NDK integration */
  public static void close() {
    loadNativeLibraries();
    shutdown();
  }

  /**
   * Loads all required native libraries. This is automatically done by {@link #init(NdkOptions)},
   * but can be called manually in case you want to preload the libraries before calling #init.
   */
  public static synchronized void loadNativeLibraries() {
    if (!nativeLibrariesLoaded) {
      // On older Android versions, it was necessary to manually call "`System.loadLibrary` on all
      // transitive dependencies before loading [the] main library."
      // The dependencies of `libsentry.so` are currently `lib{c,m,dl,log}.so`.
      // See
      // https://android.googlesource.com/platform/bionic/+/master/android-changes-for-ndk-developers.md#changes-to-library-dependency-resolution
      System.loadLibrary("log");
      System.loadLibrary("sentry");
      System.loadLibrary("sentry-android");
      nativeLibrariesLoaded = true;
    }
  }
}
