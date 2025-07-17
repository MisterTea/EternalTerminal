package io.sentry.ndk;

import org.jetbrains.annotations.Nullable;

public final class NativeModuleListLoader {

  public static native DebugImage[] nativeLoadModuleList();

  public static native void nativeClearModuleList();

  public @Nullable DebugImage[] loadModuleList() {
    return nativeLoadModuleList();
  }

  public void clearModuleList() {
    nativeClearModuleList();
  }
}
