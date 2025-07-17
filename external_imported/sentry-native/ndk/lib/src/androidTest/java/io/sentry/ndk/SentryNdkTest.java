package io.sentry.ndk;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class SentryNdkTest {

  @Test
  public void initDoesNotFail() throws IOException {
    final TemporaryFolder temporaryFolder = TemporaryFolder.builder().build();
    temporaryFolder.create();
    final File outboxPath = temporaryFolder.newFolder("outboxPath");

    final NdkOptions options =
        new NdkOptions(
            "https://key@sentry.io/proj",
            true,
            outboxPath.getAbsolutePath(),
            "1.0.0",
            "production",
            "dist",
            100,
            "io.sentry.ndk");

    // when initialized
    SentryNdk.init(options);

    // then it does not crash
  }

  @Test
  public void shutdownDoesNotFail() throws IOException {
    final TemporaryFolder temporaryFolder = TemporaryFolder.builder().build();
    temporaryFolder.create();
    final File outboxPath = temporaryFolder.newFolder("outboxPath");

    final NdkOptions options =
        new NdkOptions(
            "https://key@sentry.io/proj",
            true,
            outboxPath.getAbsolutePath(),
            "1.0.0",
            "production",
            "dist",
            100,
            "io.sentry.ndk");

    // when initialized
    SentryNdk.init(options);

    // and then closed
    SentryNdk.close();

    // it does not crash
  }

  @Test
  public void messageCaught() throws IOException {
    final TemporaryFolder temporaryFolder = TemporaryFolder.builder().build();
    temporaryFolder.create();
    final File outboxPath = temporaryFolder.newFolder("outboxPath");

    final NdkOptions options =
        new NdkOptions(
            "https://key@sentry.io/proj",
            true,
            outboxPath.getAbsolutePath(),
            "1.0.0",
            "production",
            "dist",
            100,
            "io.sentry.ndk");

    // when initialized
    SentryNdk.init(options);

    // and a message is captured
    NdkTestHelper.message();

    // then the message should be stored on disk
    File[] files = outboxPath.listFiles();
    assertNotNull(files);
    assertEquals(1, files.length);
    File firstFile = files[0];
    String content = new String(Files.readAllBytes(firstFile.toPath()), StandardCharsets.UTF_8);
    assertTrue(content.contains("It works!")); // expected message content from
    // Java_io_sentry_ndk_NdkTestHelper_message(..) in ndk-test.cpp
  }

  @Test
  public void transactionSampled() throws IOException {
    final TemporaryFolder temporaryFolder = TemporaryFolder.builder().build();
    temporaryFolder.create();
    final File outboxPath = temporaryFolder.newFolder("outboxPath");

    final NdkOptions options =
        new NdkOptions(
            "https://key@sentry.io/proj",
            true,
            outboxPath.getAbsolutePath(),
            "1.0.0",
            "production",
            "dist",
            100,
            "io.sentry.ndk");

    // set tracesSampleRate to 1
    options.setTracesSampleRate(1);

    // when initialized
    SentryNdk.init(options);

    // and a transaction is captured
    NdkTestHelper.transaction();
    // then the transaction should be stored on disk (sampled)
    File[] files = outboxPath.listFiles();
    assertNotNull(files);
    assertEquals(1, files.length);
    File firstFile = files[0];
    String content = new String(Files.readAllBytes(firstFile.toPath()), StandardCharsets.UTF_8);
    assertTrue(content.contains("\"type\":\"transaction\""));
  }

  @Test
  public void transactionNotSampled() throws IOException {
    final TemporaryFolder temporaryFolder = TemporaryFolder.builder().build();
    temporaryFolder.create();
    final File outboxPath = temporaryFolder.newFolder("outboxPath");

    final NdkOptions options =
        new NdkOptions(
            "https://key@sentry.io/proj",
            true,
            outboxPath.getAbsolutePath(),
            "1.0.0",
            "production",
            "dist",
            100,
            "io.sentry.ndk");

    // set tracesSampleRate to 0
    options.setTracesSampleRate(0);

    // when initialized
    SentryNdk.init(options);

    // and a transaction is captured
    NdkTestHelper.transaction();
    // then the transaction should not be stored on disk (not sampled)
    File[] files = outboxPath.listFiles();
    assertNotNull(files);
    assertEquals(0, files.length);
  }

  @Test
  public void shutdownDoesNotFailWhenNotInitialized() {
    // when closed without prior being initialized
    SentryNdk.close();

    // then it does not crash
  }
}
