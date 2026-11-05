using System.Runtime.InteropServices;

namespace winformsshit
{
    // has to match lossantos_options or everything explodes
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct LosSantosOptions
    {
        public string? GtaDir, GmodDir, WorkDir, KeysDir, MagicPath;
        public int Threads, Background, Flags;
        public string? VgmstreamPath, FfmpegPath;
    }

    internal static class LosSantosInstall
    {
        public const int DoSwap = 8;     // swap only
        public const int DoAll = 31;     // everything

        public const int ResultOk = 1;
        public const int ResultFailed = -1;
        public const int ResultCancelled = -2;

        // runs on dll threads, dont touch the ui from this
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void Progress(IntPtr user, IntPtr stage, int done, int total, IntPtr message);

        private const string Dll = "lossantos_install.dll";

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "lossantos_install_start")]
        public static extern int Start(ref LosSantosOptions options, Progress callback, IntPtr user);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "lossantos_install_cancel")]
        public static extern void Cancel();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "lossantos_install_wait")]
        public static extern int Wait(uint milliseconds);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "lossantos_install_result")]
        public static extern int Result();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "lossantos_install_error")]
        private static extern IntPtr ErrorPtr();

        public static string Error() => Marshal.PtrToStringUTF8(ErrorPtr()) ?? "";
    }
}
