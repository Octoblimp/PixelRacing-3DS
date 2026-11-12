# Records what the PC is playing (WASAPI loopback of the default output device) to a WAV, so
# the port's sound can be checked in Azahar without listening: tools/run_azahar.ps1 drives the
# game while this runs in another process.
#
# Usage: tools/capture_audio.ps1 -Seconds 60 -Out "$env:TEMP\pixelracing_azahar\audio.wav"
# The WAV is the device's mix format (usually 48 kHz stereo 32-bit float).
param(
    [int]$Seconds = 30,
    [string]$Out = (Join-Path $env:TEMP "pixelracing_azahar\audio.wav")
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Out) | Out-Null

Add-Type @"
using System;
using System.IO;
using System.Threading;
using System.Runtime.InteropServices;

public static class Loopback {
    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumerator {}

    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceEnumerator {
        int EnumAudioEndpoints(int dataFlow, int stateMask, out IntPtr devices);
        int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice device);
    }

    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDevice {
        int Activate(ref Guid iid, int clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
    }

    [ComImport, Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioClient {
        int Initialize(int shareMode, int streamFlags, long bufferDuration, long periodicity, IntPtr format, IntPtr sessionGuid);
        int GetBufferSize(out uint frames);
        int GetStreamLatency(out long latency);
        int GetCurrentPadding(out uint frames);
        int IsFormatSupported(int shareMode, IntPtr format, out IntPtr closest);
        int GetMixFormat(out IntPtr format);
        int GetDevicePeriod(out long def, out long min);
        int Start();
        int Stop();
        int Reset();
        int SetEventHandle(IntPtr h);
        int GetService(ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
    }

    [ComImport, Guid("C8ADBD64-E71E-48a0-A4DE-185C395CD317"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioCaptureClient {
        int GetBuffer(out IntPtr data, out uint frames, out uint flags, out ulong devPos, out ulong qpcPos);
        int ReleaseBuffer(uint frames);
        int GetNextPacketSize(out uint frames);
    }

    public static string Record(string path, int seconds) {
        var en = (IMMDeviceEnumerator)new MMDeviceEnumerator();
        IMMDevice dev;
        Marshal.ThrowExceptionForHR(en.GetDefaultAudioEndpoint(0 /* eRender */, 0 /* eConsole */, out dev));
        Guid iidClient = typeof(IAudioClient).GUID;
        object o;
        Marshal.ThrowExceptionForHR(dev.Activate(ref iidClient, 23 /* CLSCTX_ALL */, IntPtr.Zero, out o));
        var client = (IAudioClient)o;
        IntPtr fmt;
        Marshal.ThrowExceptionForHR(client.GetMixFormat(out fmt));
        short tag = Marshal.ReadInt16(fmt, 0);
        short ch = Marshal.ReadInt16(fmt, 2);
        int rate = Marshal.ReadInt32(fmt, 4);
        short align = Marshal.ReadInt16(fmt, 12);
        short bits = Marshal.ReadInt16(fmt, 14);
        short cb = Marshal.ReadInt16(fmt, 16);
        byte[] fmtBytes = new byte[18 + cb];
        Marshal.Copy(fmt, fmtBytes, 0, fmtBytes.Length);
        Marshal.ThrowExceptionForHR(client.Initialize(0 /* shared */, 0x00020000 /* LOOPBACK */,
                                                      10000000, 0, fmt, IntPtr.Zero));
        Guid iidCap = typeof(IAudioCaptureClient).GUID;
        Marshal.ThrowExceptionForHR(client.GetService(ref iidCap, out o));
        var cap = (IAudioCaptureClient)o;

        var data = new MemoryStream();
        Marshal.ThrowExceptionForHR(client.Start());
        var t0 = DateTime.UtcNow;
        long written = 0;
        while ((DateTime.UtcNow - t0).TotalSeconds < seconds) {
            Thread.Sleep(10);
            uint pkt;
            cap.GetNextPacketSize(out pkt);
            while (pkt > 0) {
                IntPtr p; uint frames, flags; ulong a, b;
                Marshal.ThrowExceptionForHR(cap.GetBuffer(out p, out frames, out flags, out a, out b));
                byte[] buf = new byte[frames * align];
                if ((flags & 2) == 0) Marshal.Copy(p, buf, 0, buf.Length); /* 2 = SILENT */
                data.Write(buf, 0, buf.Length);
                written += frames;
                cap.ReleaseBuffer(frames);
                cap.GetNextPacketSize(out pkt);
            }
            /* Loopback delivers nothing while the device is silent: pad with zeros so the
             * recording keeps real time. */
            long want = (long)((DateTime.UtcNow - t0).TotalSeconds * rate) - 4800;
            if (want > written) {
                byte[] z = new byte[(want - written) * align];
                data.Write(z, 0, z.Length);
                written = want;
            }
        }
        client.Stop();

        using (var f = new BinaryWriter(File.Create(path))) {
            byte[] pcm = data.ToArray();
            f.Write(new char[] {'R','I','F','F'});
            f.Write(4 + 8 + fmtBytes.Length + 8 + pcm.Length);
            f.Write(new char[] {'W','A','V','E','f','m','t',' '});
            f.Write(fmtBytes.Length);
            f.Write(fmtBytes);
            f.Write(new char[] {'d','a','t','a'});
            f.Write(pcm.Length);
            f.Write(pcm);
        }
        return String.Format("{0} Hz, {1} ch, {2} bit, tag 0x{3:x}, {4:F1} s", rate, ch, bits,
                             (ushort)tag, written / (double)rate);
    }
}
"@

[Loopback]::Record($Out, $Seconds)
