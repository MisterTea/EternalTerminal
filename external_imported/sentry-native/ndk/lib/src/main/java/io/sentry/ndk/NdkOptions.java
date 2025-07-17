package io.sentry.ndk;

import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

public final class NdkOptions {
  private final @NotNull String dsn;
  private final boolean isDebug;
  private final @NotNull String outboxPath;
  private final @Nullable String release;
  private final @Nullable String environment;
  private final @Nullable String dist;
  private final int maxBreadcrumbs;
  private final @Nullable String sdkName;
  private NdkHandlerStrategy ndkHandlerStrategy =
      NdkHandlerStrategy.SENTRY_HANDLER_STRATEGY_DEFAULT;
  private float tracesSampleRate = 0;

  public NdkOptions(
      @NotNull String dsn,
      boolean isDebug,
      @NotNull String outboxPath,
      @Nullable String release,
      @Nullable String environment,
      @Nullable String dist,
      int maxBreadcrumbs,
      @Nullable String sdkName) {
    this.dsn = dsn;
    this.isDebug = isDebug;
    this.outboxPath = outboxPath;
    this.release = release;
    this.environment = environment;
    this.dist = dist;
    this.maxBreadcrumbs = maxBreadcrumbs;
    this.sdkName = sdkName;
  }

  @NotNull
  public String getDsn() {
    return dsn;
  }

  public boolean isDebug() {
    return isDebug;
  }

  @NotNull
  public String getOutboxPath() {
    return outboxPath;
  }

  @Nullable
  public String getRelease() {
    return release;
  }

  @Nullable
  public String getEnvironment() {
    return environment;
  }

  @Nullable
  public String getDist() {
    return dist;
  }

  public int getMaxBreadcrumbs() {
    return maxBreadcrumbs;
  }

  @Nullable
  public String getSdkName() {
    return sdkName;
  }

  public void setNdkHandlerStrategy(final @NotNull NdkHandlerStrategy ndkHandlerStrategy) {
    this.ndkHandlerStrategy = ndkHandlerStrategy;
  }

  public int getNdkHandlerStrategy() {
    return ndkHandlerStrategy.getValue();
  }

  public void setTracesSampleRate(final float tracesSampleRate) {
    this.tracesSampleRate = tracesSampleRate;
  }

  public float getTracesSampleRate() {
    return tracesSampleRate;
  }
}
