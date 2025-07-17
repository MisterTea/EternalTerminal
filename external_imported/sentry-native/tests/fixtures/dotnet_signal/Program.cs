namespace dotnet_signal;

using System;
using System.Runtime.InteropServices;

class Program
{
    [DllImport("crash", EntryPoint = "native_crash")]
    static extern void native_crash();

    [DllImport("crash", EntryPoint = "enable_sigaltstack")]
    static extern void enable_sigaltstack();

    [DllImport("sentry", EntryPoint = "sentry_options_new")]
    static extern IntPtr sentry_options_new();

    [DllImport("sentry", EntryPoint = "sentry_options_set_handler_strategy")]
    static extern IntPtr sentry_options_set_handler_strategy(IntPtr options, int strategy);

    [DllImport("sentry", EntryPoint = "sentry_options_set_debug")]
    static extern IntPtr sentry_options_set_debug(IntPtr options, int debug);

    [DllImport("sentry", EntryPoint = "sentry_init")]
    static extern int sentry_init(IntPtr options);

    static void Main(string[] args)
    {
        var githubActions = Environment.GetEnvironmentVariable("GITHUB_ACTIONS") ?? string.Empty;
        if (githubActions == "true") {
            // Set up our own `sigaltstack` for this thread if we're running on GHA because of a failure to run any
            // signal handler after the initial setup. This behavior is locally non-reproducible and likely runner-related.
            // I ran this against .net7/8/9 on at least 10 different Linux setups, and it worked on all, but on GHA
            // it only works if we __don't__ accept the already installed `sigaltstack`.
            enable_sigaltstack();
        }

        // setup minimal sentry-native
        var options = sentry_options_new();
        sentry_options_set_handler_strategy(options, 1);
        sentry_options_set_debug(options, 1);
        sentry_init(options);

        var doNativeCrash = args is ["native-crash"];
        if (doNativeCrash)
        {
            native_crash();
        }
        else
        {
            try
            {
                Console.WriteLine("dereference a NULL object from managed code");
                var s = default(string);
                var c = s.Length;
            }
            catch (NullReferenceException exception)
            {
                Console.WriteLine("dereference another NULL object from managed code");
                var s = default(string);
                var c = s.Length;
            }
        }
    }
}