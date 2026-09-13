using System;
using System.Runtime.InteropServices;

// Mirrors pointreceiver_status in pointreceiver.h
public enum PointreceiverStatus
{
    Ok = 0,
    ErrorInvalidArgument,
    ErrorAlreadyRunning,
    ErrorNotRunning,
    ErrorConnectionFailed,
    ErrorTimeout,
    ErrorDecodeFailed,
    ErrorOutOfRange,
    ErrorOutOfMemory,
    ErrorInternal
}

// Mirrors pointreceiver_attribute_type
public enum PointreceiverAttributeType
{
    Float32 = 0,
    UInt8,
    UInt16,
    UInt32,
    Int16,
    Int32
}

// One named per-point attribute, mirroring pointreceiver_attribute
//
// ElementType is how the values are stored.
// If ElementType is not a float, multiply the raw value by 
// quantisation_step to get the real output
[StructLayout(LayoutKind.Sequential)]
public struct PointreceiverAttribute
{
    public IntPtr name;
    public IntPtr data;
    public UIntPtr element_count; // size_t
    public float quantisation_step;
    public uint component_count;
    public uint stride;
    public PointreceiverAttributeType element_type;

    public string Name => Marshal.PtrToStringAnsi(name);
    public int ElementCount => (int)element_count.ToUInt64();
}

// A point cloud frame received from pointcaster
//
// Mirrors pointreceiver_point_cloud_frame
//
//  positions:  pointreceiver_position_t[], four int16s per point
//  colours:    pointreceiver_color_t[], four bytes per point
//  attributes: pointreceiver_attribute[], attribute_count entries
//
// Operators decide which attributes exist, so the set can change between
// frames on one channel. Look them up by name rather than caching an index.
[StructLayout(LayoutKind.Sequential)]
public struct PointCloudFrame
{
    public UIntPtr point_count; // size_t
    public IntPtr positions;
    public IntPtr colours;
    public IntPtr attributes;
    public UIntPtr attribute_count; // size_t

    public int PointCount => (int)point_count.ToUInt64();
    public int AttributeCount => (int)attribute_count.ToUInt64();

    public PointreceiverAttribute GetAttribute(int index)
    {
        var offset = index * Marshal.SizeOf<PointreceiverAttribute>();
        return Marshal.PtrToStructure<PointreceiverAttribute>(
            IntPtr.Add(attributes, offset));
    }

    public bool TryGetAttribute(string name, out PointreceiverAttribute found)
    {
        for (var i = 0; i < AttributeCount; i++)
        {
            found = GetAttribute(i);
            if (found.Name == name) return true;
        }
        found = default;
        return false;
    }
}
