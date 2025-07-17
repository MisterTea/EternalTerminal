package io.sentry.ndk;

import java.util.Map;
import org.jetbrains.annotations.Nullable;

public final class DebugImage {

  /**
   * The unique UUID of the image.
   *
   * <p>UUID computed from the file contents, assigned by the Java SDK.
   */
  private @Nullable String uuid;

  private @Nullable String type;

  /**
   * Unique debug identifier of the image.
   *
   * <p>- `elf`: Debug identifier of the dynamic library or executable. If a code identifier is
   * available, the debug identifier is the little-endian UUID representation of the first 16-bytes
   * of that identifier. Spaces are inserted for readability, note the byte order of the first
   * fields:
   *
   * <p>```text code id: f1c3bcc0 2798 65fe 3058 404b2831d9e6 4135386c debug id:
   * c0bcc3f1-9827-fe65-3058-404b2831d9e6 ```
   *
   * <p>If no code id is available, the debug id should be computed by XORing the first 4096 bytes
   * of the `.text` section in 16-byte chunks, and representing it as a little-endian UUID (again
   * swapping the byte order).
   *
   * <p>- `pe`: `signature` and `age` of the PDB file. Both values can be read from the CodeView
   * PDB70 debug information header in the PE. The value should be represented as little-endian
   * UUID, with the age appended at the end. Note that the byte order of the UUID fields must be
   * swapped (spaces inserted for readability):
   *
   * <p>```text signature: f1c3bcc0 2798 65fe 3058 404b2831d9e6 age: 1 debug_id:
   * c0bcc3f1-9827-fe65-3058-404b2831d9e6-1 ```
   *
   * <p>- `macho`: Identifier of the dynamic library or executable. It is the value of the `LC_UUID`
   * load command in the Mach header, formatted as UUID.
   */
  private @Nullable String debugId;

  /**
   * Path and name of the debug companion file.
   *
   * <p>- `elf`: Name or absolute path to the file containing stripped debug information for this
   * image. This value might be _required_ to retrieve debug files from certain symbol servers.
   *
   * <p>- `pe`: Name of the PDB file containing debug information for this image. This value is
   * often required to retrieve debug files from specific symbol servers.
   *
   * <p>- `macho`: Name or absolute path to the dSYM file containing debug information for this
   * image. This value might be required to retrieve debug files from certain symbol servers.
   */
  private @Nullable String debugFile;

  /**
   * Optional identifier of the code file.
   *
   * <p>- `elf`: If the program was compiled with a relatively recent compiler, this should be the
   * hex representation of the `NT_GNU_BUILD_ID` program header (type `PT_NOTE`), or the value of
   * the `.note.gnu.build-id` note section (type `SHT_NOTE`). Otherwise, leave this value empty.
   *
   * <p>Certain symbol servers use the code identifier to locate debug information for ELF images,
   * in which case this field should be included if possible.
   *
   * <p>- `pe`: Identifier of the executable or DLL. It contains the values of the `time_date_stamp`
   * from the COFF header and `size_of_image` from the optional header formatted together into a hex
   * string using `%08x%X` (note that the second value is not padded):
   *
   * <p>```text time_date_stamp: 0x5ab38077 size_of_image: 0x9000 code_id: 5ab380779000 ```
   *
   * <p>The code identifier should be provided to allow server-side stack walking of binary crash
   * reports, such as Minidumps.
   *
   * <p>
   *
   * <p>- `macho`: Identifier of the dynamic library or executable. It is the value of the `LC_UUID`
   * load command in the Mach header, formatted as UUID. Can be empty for Mach images, as it is
   * equivalent to the debug identifier.
   */
  private @Nullable String codeId;

  /**
   * Path and name of the image file (required).
   *
   * <p>The absolute path to the dynamic library or executable. This helps to locate the file if it
   * is missing on Sentry.
   *
   * <p>- `pe`: The code file should be provided to allow server-side stack walking of binary crash
   * reports, such as Minidumps.
   */
  private @Nullable String codeFile;

  /**
   * Starting memory address of the image (required).
   *
   * <p>Memory address, at which the image is mounted in the virtual address space of the process.
   * Should be a string in hex representation prefixed with `"0x"`.
   */
  private @Nullable String imageAddr;

  /**
   * Size of the image in bytes (required).
   *
   * <p>The size of the image in virtual memory. If missing, Sentry will assume that the image spans
   * up to the next image, which might lead to invalid stack traces.
   */
  private @Nullable Long imageSize;

  /**
   * CPU architecture target.
   *
   * <p>Architecture of the module. If missing, this will be backfilled by Sentry.
   */
  private @Nullable String arch;

  @SuppressWarnings("unused")
  private @Nullable Map<String, Object> unknown;

  public @Nullable String getUuid() {
    return uuid;
  }

  public void setUuid(final @Nullable String uuid) {
    this.uuid = uuid;
  }

  public @Nullable String getType() {
    return type;
  }

  public void setType(final @Nullable String type) {
    this.type = type;
  }

  public @Nullable String getDebugId() {
    return debugId;
  }

  public void setDebugId(final @Nullable String debugId) {
    this.debugId = debugId;
  }

  public @Nullable String getDebugFile() {
    return debugFile;
  }

  public void setDebugFile(final @Nullable String debugFile) {
    this.debugFile = debugFile;
  }

  public @Nullable String getCodeFile() {
    return codeFile;
  }

  public void setCodeFile(final @Nullable String codeFile) {
    this.codeFile = codeFile;
  }

  public @Nullable String getImageAddr() {
    return imageAddr;
  }

  public void setImageAddr(final @Nullable String imageAddr) {
    this.imageAddr = imageAddr;
  }

  public @Nullable Long getImageSize() {
    return imageSize;
  }

  public void setImageSize(final @Nullable Long imageSize) {
    this.imageSize = imageSize;
  }

  /**
   * Sets the image size.
   *
   * @param imageSize the image size.
   */
  public void setImageSize(long imageSize) {
    this.imageSize = imageSize;
  }

  public @Nullable String getArch() {
    return arch;
  }

  public void setArch(final @Nullable String arch) {
    this.arch = arch;
  }

  public @Nullable String getCodeId() {
    return codeId;
  }

  public void setCodeId(final @Nullable String codeId) {
    this.codeId = codeId;
  }
}
