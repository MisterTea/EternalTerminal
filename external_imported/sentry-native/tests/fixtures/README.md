The files here are created like this:

    $ gcc -shared -o with-buildid.so test.c
    $ gcc -Wl,--build-id=none -shared -o without-buildid.so test.c

For `with-buildid.so`, `sentry-cli difutil check` outputs the following:

    > debug_id: 4247301c-14f1-5421-53a8-a777f6cdb3a2 (x86_64)
    > code_id: 1c304742f114215453a8a777f6cdb3a2b8505e11 (x86_64)

For `without-buildid.so`:

    > debug_id: 29271919-a2ef-129d-9aac-be85a0948d9c (x86_64)

This is the value we want to replicate with our fallback hashing

Similarly for `libstdc++.so`, which is taken from the x86 Android API-16 image.

    > debug_id: 7fa824da-38f1-b87c-04df-718fda64990c (x86)

And `sentry_example`, which is from an x86 Android API-16 build.

    > debug_id: 6c4ac2b4-95c9-7fc1-b18a-65184a65863c (x86)
    > code_id: b4c24a6cc995c17fb18a65184a65863cfc01c673 (x86)
