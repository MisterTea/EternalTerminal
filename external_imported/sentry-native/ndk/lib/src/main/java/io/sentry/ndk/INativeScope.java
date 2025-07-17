package io.sentry.ndk;

public interface INativeScope {
  void setTag(String key, String value);

  void removeTag(String key);

  void setExtra(String key, String value);

  void removeExtra(String key);

  void setUser(String id, String email, String ipAddress, String username);

  void removeUser();

  void addBreadcrumb(
      String level, String message, String category, String type, String timestamp, String data);

  void setTrace(String traceId, String parentSpanId);
}
