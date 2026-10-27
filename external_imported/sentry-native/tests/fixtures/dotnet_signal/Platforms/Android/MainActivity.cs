using Android.App;
using Android.OS;

// Required for "adb shell run-as" to access the app's data directory in Release builds
[assembly: Application(Debuggable = true)]

namespace dotnet_signal;

[Activity(Name = "dotnet_signal.MainActivity", MainLauncher = true)]
public class MainActivity : Activity
{
    static MainActivity()
    {
        Java.Lang.JavaSystem.LoadLibrary("sentry");
        Java.Lang.JavaSystem.LoadLibrary("crash");
    }

    protected override void OnResume()
    {
        base.OnResume();

        var arg = Intent?.GetStringExtra("arg");
        var strategy = Intent?.GetStringExtra("strategy") ?? "";
        var reinit = Intent?.GetStringExtra("reinit") ?? "";
        if (!string.IsNullOrEmpty(arg))
        {
            var databasePath = FilesDir?.AbsolutePath + "/.sentry-native";
            var args = new List<string> { arg };
            if (!string.IsNullOrEmpty(strategy))
                args.Add(strategy);
            if (!string.IsNullOrEmpty(reinit))
                args.Add("reinit");

            // Post to the message queue so the activity finishes starting
            // before the crash test runs. Without this, "am start -W" may hang.
            new Handler(Looper.MainLooper!).Post(() =>
            {
                Program.RunTest(args.ToArray(), databasePath);
                FinishAndRemoveTask();
                Java.Lang.JavaSystem.Exit(0);
            });
        }
    }
}
