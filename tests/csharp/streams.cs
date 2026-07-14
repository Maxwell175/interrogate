// Test driver for interrogate's C# stream bridge.  Exercises
// System.IO.Stream -> C++ std::istream / std::ostream / std::iostream
// marshalling through the generated bindings.

using System;
using System.IO;
using System.Text;
using Streams;

internal static class Driver {
    private static int _failed;

    private static void Check(bool ok, string msg) {
        Console.WriteLine((ok ? "  PASS " : "  FAIL ") + msg);
        if (!ok) _failed++;
    }

    public static int Main() {
        TestWriteHello();
        TestReadAll();
        TestEchoIostream();
        TestReturnedStream();

        Console.WriteLine();
        if (_failed == 0) {
            Console.WriteLine("All stream-bridge tests passed.");
            return 0;
        }
        Console.WriteLine($"{_failed} test(s) failed.");
        return 1;
    }

    // StreamUser.write_hello(ostream &) writes "hello-<id>" to the stream.
    // We pass a MemoryStream and verify the bytes arrive correctly.  This
    // exercises the ostream bridge + BridgeBuf::overflow/sync on the C++ side.
    private static void TestWriteHello() {
        Console.WriteLine("TestWriteHello: ostream parameter receives C++ writes");
        using var user = new StreamUser();
        user.set_id(42);

        using var mem = new MemoryStream();
        user.write_hello(mem);

        string got = Encoding.UTF8.GetString(mem.ToArray());
        Check(got == "hello-42", $"wrote 'hello-42' (got: '{got}')");
    }

    // StreamUser.read_all(istream &) consumes all bytes from the stream and
    // records the count.  Exercises the istream bridge + BridgeBuf::underflow.
    private static void TestReadAll() {
        Console.WriteLine("TestReadAll: istream parameter delivers managed bytes to C++");
        using var user = new StreamUser();

        byte[] payload = new byte[12345];
        for (int i = 0; i < payload.Length; i++) payload[i] = (byte)(i & 0xFF);
        using var mem = new MemoryStream(payload);

        user.read_all(mem);
        Check(user.bytes_read() == payload.Length,
              $"bytes_read() == {payload.Length} (got: {user.bytes_read()})");
    }

    // StreamUser.echo(iostream &) reads a line from the stream and writes
    // "echo:<line>" back.  Exercises iostream (read + write + seek) on a
    // single bridge.
    private static void TestEchoIostream() {
        Console.WriteLine("TestEchoIostream: iostream parameter round-trips through C++");
        using var user = new StreamUser();

        using var mem = new MemoryStream();
        using (var writer = new StreamWriter(mem, leaveOpen: true)) {
            writer.Write("world\n");
        }
        mem.Position = 0;

        user.echo(mem);

        mem.Position = 0;
        using var reader = new StreamReader(mem);
        string got = reader.ReadToEnd();
        Check(got.Contains("echo:world"), $"echo produced '...echo:world...' (got: '{got}')");
    }

    // A stream C++ owns, handed back and then taken back to free.  Before reverse
    // bridging this was untestable: open_stream() returned an opaque IntPtr, and
    // close_stream() took the *parameter* path, which bridges a brand-new native
    // stream around a managed one -- so C++ deleted a stream it had never handed out
    // while the bridge deleted it again.  Both directions are exercised here.
    private static void TestReturnedStream() {
        Console.WriteLine("TestReturnedStream: C++ hands back a stream, then frees it");
        long before = Interrogate.StreamBridge.LiveCount;

        var user = new StreamUser();
        Stream? stream = user.open_stream();
        Check(stream != null, "open_stream() returned a System.IO.Stream");
        Check(stream is Interrogate.NativeStream, "it is a NativeStream over the C++ istream");
        Check(stream!.CanRead && !stream.CanWrite, "readable, not writable");

        using (var reader = new StreamReader(stream, leaveOpen: true)) {
            string got = reader.ReadToEnd();
            Check(got == "native-stream-contents", $"read the contents (got: '{got}')");
        }

        // Must close the stream C++ actually gave us -- not a bridged impostor.
        user.close_stream(stream);
        Check(true, "close_stream() did not double free");

        Check(Interrogate.StreamBridge.LiveCount == before,
              $"no bridge was created for a NativeStream ({before} -> {Interrogate.StreamBridge.LiveCount})");
    }
}
