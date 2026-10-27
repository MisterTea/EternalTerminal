package io.sentry.ndk;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class SentryNdkTest {

  private static final class ProcMapEntry {
    private final long start;
    private final long end;
    private final String line;

    private ProcMapEntry(final long start, final long end, final String line) {
      this.start = start;
      this.end = end;
      this.line = line;
    }
  }

  private static int compareUnsignedLong(final long left, final long right) {
    final long biasedLeft = left ^ Long.MIN_VALUE;
    final long biasedRight = right ^ Long.MIN_VALUE;
    if (biasedLeft == biasedRight) {
      return 0;
    }
    return biasedLeft < biasedRight ? -1 : 1;
  }

  private static long parseHexLong(final String hex) {
    long result = 0;
    for (int i = 0; i < hex.length(); i++) {
      final int digit = Character.digit(hex.charAt(i), 16);
      if (digit < 0) {
        throw new NumberFormatException(hex);
      }
      result = (result << 4) | digit;
    }
    return result;
  }

  private static boolean containsAddress(final long address, final long start, final long end) {
    return compareUnsignedLong(address, start) >= 0 && compareUnsignedLong(address, end) < 0;
  }

  private static String readFile(final File file) throws IOException {
    final ByteArrayOutputStream out = new ByteArrayOutputStream();
    final byte[] buf = new byte[4096];
    final FileInputStream in = new FileInputStream(file);
    try {
      int read;
      while ((read = in.read(buf)) != -1) {
        out.write(buf, 0, read);
      }
    } finally {
      in.close();
    }
    return out.toString("UTF-8");
  }

  private static ProcMapEntry findProcMapEntryContaining(final long address) throws IOException {
    final BufferedReader reader =
        new BufferedReader(new InputStreamReader(new FileInputStream("/proc/self/maps"), "UTF-8"));
    try {
      String line;
      while ((line = reader.readLine()) != null) {
        final int dash = line.indexOf('-');
        final int space = line.indexOf(' ', dash + 1);
        if (dash < 1 || space < 0) {
          continue;
        }

        final long start = parseHexLong(line.substring(0, dash));
        final long end = parseHexLong(line.substring(dash + 1, space));
        if (containsAddress(address, start, end)) {
          return new ProcMapEntry(start, end, line);
        }
      }
    } finally {
      reader.close();
    }
    return null;
  }

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

  @Test(expected = IllegalStateException.class)
  public void initThrowsException() throws IOException {
    final TemporaryFolder temporaryFolder = TemporaryFolder.builder().build();
    temporaryFolder.create();
    final File outboxPath = temporaryFolder.newFolder("outboxPath");

    //noinspection DataFlowIssue
    final NdkOptions options =
        new NdkOptions(
            null,
            true,
            outboxPath.getAbsolutePath(),
            "1.0.0",
            "production",
            "dist",
            100,
            "io.sentry.ndk");

    // when initialized with a NULL dsn
    SentryNdk.init(options);

    // then it does crash
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

    // and a breadcrumb whose data was serialized to a JSON object string is added
    new NativeScope()
        .addBreadcrumb(
            "info", "test crumb", "test", "default", null, "{\"some_key\":\"some_value\"}");

    // and a message is captured (which merges the scope's breadcrumbs into it)
    NdkTestHelper.message();

    // then the message should be stored on disk
    File[] files = outboxPath.listFiles();
    assertNotNull(files);
    assertEquals(1, files.length);
    File firstFile = files[0];
    String content = readFile(firstFile);
    assertTrue(content.contains("It works!")); // expected message content from
    // Java_io_sentry_ndk_NdkTestHelper_message(..) in ndk-test.cpp

    // and the breadcrumb data is well formed.
    assertTrue(content.contains("\"some_key\":\"some_value\""));
    assertFalse(content.contains("\"data\":\"{"));

    // and native events carry module debug images.
    assertTrue(content.contains("\"debug_meta\""));
    assertTrue(content.contains("\"images\":[{"));
  }

  @Test
  public void nativeModuleListLoadsDebugImages() throws IOException {
    SentryNdk.loadNativeLibraries();

    final long sentryAddress = NdkTestHelper.sentryGetModulesListAddress();
    final ProcMapEntry sentryProcMapEntry = findProcMapEntryContaining(sentryAddress);
    assertNotNull("/proc/self/maps should contain the loaded libsentry symbol", sentryProcMapEntry);

    final NativeModuleListLoader loader = new NativeModuleListLoader();
    loader.clearModuleList();

    final DebugImage[] images = loader.loadModuleList();

    assertNotNull(images);
    assertTrue(images.length > 0);

    DebugImage sentryImage = null;
    long sentryImageStart = 0;
    long sentryImageEnd = 0;
    for (DebugImage image : images) {
      final String imageAddr = image.getImageAddr();
      final Long imageSize = image.getImageSize();
      if (imageAddr == null || imageSize == null || imageSize <= 0) {
        continue;
      }
      final long start = parseHexLong(imageAddr.replace("0x", ""));
      final long end = start + imageSize;
      if (containsAddress(sentryAddress, start, end)) {
        sentryImage = image;
        sentryImageStart = start;
        sentryImageEnd = end;
        break;
      }
    }

    assertNotNull(
        "module list should contain the loaded libsentry image from " + sentryProcMapEntry.line,
        sentryImage);
    assertEquals("elf", sentryImage.getType());
    assertNotNull(sentryImage.getCodeFile());
    assertNotNull(sentryImage.getDebugId());
    assertNotNull(sentryImage.getCodeId());
    assertTrue(compareUnsignedLong(sentryImageStart, sentryProcMapEntry.start) <= 0);
    assertTrue(compareUnsignedLong(sentryImageEnd, sentryProcMapEntry.end) >= 0);
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
    String content = readFile(firstFile);
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
