using System;
using System.Runtime.InteropServices;
using System.Text;

// Bindings for the Pointrecevier C API
public static class PointreceiverNative
{

#if  UNITY_EDITOR
        public const string NativeLib = "pointreceiver";
#elif UNITY_STANDALONE_WIN
        public const string NativeLib = "pointreceiver";
#else
        public const string NativeLib = "pointreceiver";
        // public const string NativeLib = "pointreceiverd";
#endif

    [DllImport(NativeLib, EntryPoint = "pointreceiver_create_context",
     CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr CreateContext();

    [DllImport(NativeLib, EntryPoint = "pointreceiver_destroy_context",
     CallingConvention = CallingConvention.Cdecl)]
    public static extern void DestroyContext(IntPtr context);

    [DllImport(NativeLib, EntryPoint = "pointreceiver_set_client_name",
     CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public static extern PointreceiverStatus SetClientName(IntPtr context, string clientName);

    [DllImport(NativeLib, EntryPoint = "pointreceiver_start_point_receiver",
     CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public static extern PointreceiverStatus StartPointReceiver(IntPtr context, string pointcasterAddress);

    [DllImport(NativeLib, EntryPoint = "pointreceiver_stop_point_receiver",
     CallingConvention = CallingConvention.Cdecl)]
    public static extern PointreceiverStatus StopPointReceiver(IntPtr context);

    [DllImport(NativeLib, EntryPoint = "pointreceiver_point_receiver_running",
     CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.I1)]
    public static extern bool PointReceiverRunning(IntPtr context);

    // Subscribe/unsubscribe to a point cloud channel by address.
    // A null or empty address subscribes to all channels.
    [DllImport(NativeLib, EntryPoint = "pointreceiver_subscribe_to_point_cloud",
     CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public static extern PointreceiverStatus SubscribeToPointCloud(IntPtr context, string address);

    [DllImport(NativeLib, EntryPoint = "pointreceiver_unsubscribe_from_point_cloud",
     CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public static extern PointreceiverStatus UnsubscribeFromPointCloud(IntPtr context, string address);

    // Dequeue the oldest pending frame across every subscribed channel, along
    // with the address of the channel it arrived on.
    [DllImport(NativeLib, EntryPoint = "pointreceiver_dequeue_point_cloud",
     CallingConvention = CallingConvention.Cdecl)]
    public static extern PointreceiverStatus DequeuePointCloud(IntPtr context,
        [Out, MarshalAs(UnmanagedType.LPStr)] StringBuilder outAddress, UIntPtr addressCapacity,
        ref PointCloudFrame frame, int timeoutMs);

    // Channel addresses observed on the stream so far
    [DllImport(NativeLib, EntryPoint = "pointreceiver_known_point_cloud_address_count",
     CallingConvention = CallingConvention.Cdecl)]
    public static extern UIntPtr KnownPointCloudAddressCount(IntPtr context);

    [DllImport(NativeLib, EntryPoint = "pointreceiver_get_known_point_cloud_address",
     CallingConvention = CallingConvention.Cdecl)]
    public static extern PointreceiverStatus GetKnownPointCloudAddress(IntPtr context, UIntPtr index,
        [Out, MarshalAs(UnmanagedType.LPStr)] StringBuilder outAddress, UIntPtr outCapacity);

    // Attribute lookup by name within a dequeued frame. Returns a borrowed
    // pointer into the attribute's buffer
    [DllImport(NativeLib, EntryPoint = "pointreceiver_find_attribute",
     CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public static extern IntPtr FindAttribute(ref PointCloudFrame frame, string name);

    // Converts an attribute out as floats with any quantisation already decoded.
    [DllImport(NativeLib, EntryPoint = "pointreceiver_attribute_copy_floats",
     CallingConvention = CallingConvention.Cdecl)]
    public static extern PointreceiverStatus AttributeCopyFloats(ref PointreceiverAttribute attribute,
        float[] outValues, UIntPtr outCapacity);
}
