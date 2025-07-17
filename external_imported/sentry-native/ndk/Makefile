.PHONY: all clean compile format api check

all: clean format compile api check

clean:
	./gradlew clean

compile:
	./gradlew build

# Format code using the spotless gradle plugin
format:
	./gradlew spotlessApply

# Creates the .api dump file
api:
	./gradlew :sentry-native-ndk:apiDump

# Run tests and lint
check:
	./gradlew :sentry-native-ndk:check
