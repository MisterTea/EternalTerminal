package io.sentry.ndk;

public final class NativeScope implements INativeScope {
  public static native void nativeSetTag(String key, String value);

  public static native void nativeRemoveTag(String key);

  public static native void nativeSetExtra(String key, String value);

  public static native void nativeRemoveExtra(String key);

  public static native void nativeSetUser(
      String id, String email, String ipAddress, String username);

  public static native void nativeRemoveUser();

  public static native void nativeAddBreadcrumb(
      String level, String message, String category, String type, String timestamp, String data);

  public static native void nativeSetTrace(String traceId, String parentSpanId);

  @Override
  public void setTag(String key, String value) {
    nativeSetTag(key, value);
  }

  @Override
  public void removeTag(String key) {
    nativeRemoveTag(key);
  }

  @Override
  public void setExtra(String key, String value) {
    nativeSetExtra(key, value);
  }

  @Override
  public void removeExtra(String key) {
    nativeRemoveExtra(key);
  }

  @Override
  public void setUser(String id, String email, String ipAddress, String username) {
    nativeSetUser(id, email, ipAddress, username);
  }

  @Override
  public void removeUser() {
    nativeRemoveUser();
  }

  @Override
  public void addBreadcrumb(
      String level, String message, String category, String type, String timestamp, String data) {
    nativeAddBreadcrumb(level, message, category, type, timestamp, data);
  }

  /**
   * Set the trace. The primary use for this is to allow other SDKs to propagate their trace context
   * to connect events on all layers
   */
  @Override
  public void setTrace(String traceId, String parentSpanId) {
    nativeSetTrace(traceId, parentSpanId);
  }
}
