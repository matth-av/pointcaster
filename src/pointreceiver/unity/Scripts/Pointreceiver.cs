using System;
using System.Collections.Generic;
using System.Text;

public class Pointreceiver : IDisposable
{
    private IntPtr context;

    public Pointreceiver(string clientName)
    {
        context = PointreceiverNative.CreateContext();
        if (context == IntPtr.Zero)
            throw new Exception("Failed to create context.");

        PointreceiverNative.SetClientName(context, clientName);

        UnityEngine.Debug.Log("Pointreceiver: context created");
    }

    public void StartPointReceiver(string pointcasterAddress)
    {
        var status = PointreceiverNative.StartPointReceiver(context, pointcasterAddress);
        if (status != PointreceiverStatus.Ok)
            throw new Exception($"Failed to start point receiver: {status}");

        UnityEngine.Debug.Log("Pointreceiver: point receiver started");
    }

    public void StopPointReceiver()
    {
        var status = PointreceiverNative.StopPointReceiver(context);
        if (status != PointreceiverStatus.Ok)
            UnityEngine.Debug.Log($"Warning: Stopping point receiver returned {status}.");

        UnityEngine.Debug.Log("Pointreceiver: point receiver stopped");
    }

    public bool PointReceiverRunning =>
        context != IntPtr.Zero && PointreceiverNative.PointReceiverRunning(context);

    // Subscribing is what makes frames flow at all. A null or empty address
    // takes every channel pointcaster publishes.
    public void SubscribeToPointCloud(string address)
    {
        var status = PointreceiverNative.SubscribeToPointCloud(context, address ?? "");
        if (status != PointreceiverStatus.Ok)
            UnityEngine.Debug.Log($"Warning: Subscribing to '{address}' returned {status}.");
    }

    public void UnsubscribeFromPointCloud(string address)
    {
        var status = PointreceiverNative.UnsubscribeFromPointCloud(context, address ?? "");
        if (status != PointreceiverStatus.Ok)
            UnityEngine.Debug.Log($"Warning: Unsubscribing from '{address}' returned {status}.");
    }

    // capacity of the buffers we hand the native library for channel addresses
    private const int AddressCapacity = 256;

    private readonly byte[] _AddressBuffer = new byte[AddressCapacity];

    // The frame's buffers are borrowed from the native context and are
    // invalidated by the next dequeue, so callers must copy out of them before
    // calling this again.
    public bool TryDequeuePointCloud(int timeoutMs, out string address, out PointCloudFrame frame)
    {
        frame = default;
        var status = PointreceiverNative.DequeuePointCloud(
            context, _AddressBuffer, (UIntPtr)_AddressBuffer.Length,
            ref frame, timeoutMs);
        bool success = status == PointreceiverStatus.Ok;
        address = success ? DecodeAddress(_AddressBuffer) : "";
        return success;
    }

    public List<string> KnownPointCloudAddresses()
    {
        var count = PointreceiverNative.KnownPointCloudAddressCount(context).ToUInt64();
        var addresses = new List<string>((int)count);
        var buffer = new byte[AddressCapacity];
        for (ulong i = 0; i < count; i++)
        {
            var status = PointreceiverNative.GetKnownPointCloudAddress(
                context, (UIntPtr)i, buffer, (UIntPtr)buffer.Length);
            if (status == PointreceiverStatus.Ok) addresses.Add(DecodeAddress(buffer));
        }
        return addresses;
    }

    private static string DecodeAddress(byte[] buffer)
    {
        var length = Array.IndexOf(buffer, (byte)0);
        return Encoding.UTF8.GetString(buffer, 0, length < 0 ? buffer.Length : length);
    }

    public void Dispose()
    {
        if (context == IntPtr.Zero) return;
        if (PointreceiverNative.PointReceiverRunning(context)) StopPointReceiver();
        PointreceiverNative.DestroyContext(context);
        context = IntPtr.Zero;
    }
}
