using System;
using System.IO;

internal static class ManagedCli
{
    private static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--exit37")
            return 37;
        if (args.Length != 4 || args[0] != "two words" || args[1] != "" ||
            args[2] != "quote\"value" || args[3] != "tail\\")
            return 90;
        if (new DirectoryInfo(Environment.CurrentDirectory).Name != "managed work")
            return 91;
        Console.WriteLine("managed arguments passed");
        return 0;
    }
}
