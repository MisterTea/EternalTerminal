package io.sentry.ndk;

public enum NdkHandlerStrategy {
  // Needs to match sentry_handler_strategy_t
  SENTRY_HANDLER_STRATEGY_DEFAULT(0),
  SENTRY_HANDLER_STRATEGY_CHAIN_AT_START(1);

  private final int value;

  NdkHandlerStrategy(final int value) {
    this.value = value;
  }

  public int getValue() {
    return value;
  }
}
