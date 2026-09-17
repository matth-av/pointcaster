using System;
using System.Linq;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Unity.Burst;
using Unity.Collections;
using Unity.Collections.LowLevel.Unsafe;
using Unity.Jobs;
using UnityEngine;
using UnityEngine.Rendering;

public class PointreceiverMeshHost : MonoBehaviour
{
    public string PointCasterAddress = "tcp://127.0.0.1";
    public int PointCloudPort = 9992;

    public ChannelSubscription Subscription = new ChannelSubscription();

    public enum PointShape { Square, Disk }
    public enum PointSizeMode { WorldUnits, ScreenPixels }

    [Header("Points")]
    [Tooltip("Size in metres when Size Mode is World Units, otherwise pixels. "
        + "Multiplies each point's point_scale radius in millimetres when the cloud has one.")]
    [Min(0f)] public float PointSize = 0.01f;

    public PointSizeMode SizeMode = PointSizeMode.WorldUnits;

    [Tooltip("Square is cheapest.")]
    public PointShape Shape = PointShape.Square;

    public Color Tint = new Color(0.5f, 0.5f, 0.5f, 1f);

    [Serializable]
    public class PointCloudChannel
    {
        public string Address;
        public GameObject GameObject;
        public Mesh Mesh;
    }

    public List<PointCloudChannel> PointCloudChannels = new List<PointCloudChannel>();

    private Pointreceiver Pointreceiver;
    private string SubscribedAddress;

    // every point becomes a camera facing quad, expanded in the vertex shader
    private const int VerticesPerPoint = 4;
    private const int IndicesPerPoint = 6;

    // the receiver sends positions as signed millimetres in a 16 bit integer
    private const float MillimetresToMetres = 0.001f;
    private const float PositionScale = short.MaxValue * MillimetresToMetres;

    private const float PointScaleStepsPerMillimetre = 10f;
    private const float PointScaleRange = short.MaxValue / PointScaleStepsPerMillimetre;
    private const short NoPointScale = -short.MaxValue;

    [StructLayout(LayoutKind.Sequential)]
    private struct PackedPosition
    {
        public short X, Y, Z, _Padding;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PointVertex
    {
        public short X, Y, Z;
        public short Scale;
        public Color32 Color;
    }

    private int CurrentCapacity = 0;
    private int[] Indices;
    private NativeArray<PointVertex> OutputVertices;
    private NativeArray<Vector2> QuadCorners;
    private NativeArray<Vector3> MinMax;
    private NativeArray<float> MaxPointScale;

    private const string PointShaderResource = "PointQuad";

    private Material PointMaterial;

    private float AppliedPointSize = float.NaN;
    private PointSizeMode AppliedSizeMode;
    private PointShape AppliedShape;
    private Color AppliedTint;

    void OnEnable()
    {
        if (!SystemInfo.SupportsVertexAttributeFormat(VertexAttributeFormat.SNorm16, 4)) 
        {
            Debug.LogError("Pointreceiver Mesh Host: this device reports no support for "
                + "SNorm16 vertex positions, so point clouds will not draw");
        }
        try
        {
            if (Pointreceiver == null)
                Pointreceiver = new Pointreceiver($"unity.{Environment.MachineName}");

            Pointreceiver.StartPointReceiver($"{PointCasterAddress}:{PointCloudPort}");
            SubscribedAddress = Subscription.SubscriptionAddress;
            Pointreceiver.SubscribeToPointCloud(SubscribedAddress);
        }
        catch (Exception e)
        {
            Debug.Log("Pointreceiver Mesh Host");
            Debug.LogError(e);
        }
    }

    void OnDisable()
    {
        try
        {
            if (SubscribedAddress != null)
            {
                Pointreceiver.UnsubscribeFromPointCloud(SubscribedAddress);
                SubscribedAddress = null;
            }
            Pointreceiver.StopPointReceiver();
        }
        catch
        {
            Debug.Log("Failed to stop Receiver threads");
        }
    }

    void OnDestroy()
    {
        if (PointMaterial != null) Destroy(PointMaterial);

        if (OutputVertices.IsCreated) OutputVertices.Dispose();
        if (QuadCorners.IsCreated) QuadCorners.Dispose();
        if (MinMax.IsCreated) MinMax.Dispose();
        if (MaxPointScale.IsCreated) MaxPointScale.Dispose();

        Pointreceiver?.Dispose();
        Pointreceiver = null;
    }

    void Update()
    {
        if (Pointreceiver == null) return;

        // the receiver keeps only the newest frame per channel, so draining
        // gives us at most one frame for each of them. each frame's buffers are
        // borrowed and die on the next dequeue, so unpack before looping.
        while (Pointreceiver.TryDequeuePointCloud(0, out string address, out PointCloudFrame frame))
        {
            if (frame.PointCount == 0)
            {
                ClearChannelMesh(address);
                continue;
            }
            // grow first so a mesh created below is laid out only once
            EnsureCapacity(frame.PointCount);
            var mesh = EnsureOrCreateChannelMesh(address);
            UnpackPointCloudIntoMesh(frame, mesh);
        }

        ApplyPointSettings();
    }

    Material EnsurePointMaterial()
    {
        if (PointMaterial != null) return PointMaterial;

        var shader = Resources.Load<Shader>(PointShaderResource);
        if (shader == null)
        {
            Debug.LogError("Pointreceiver Mesh Host: could not load the point "
                + "shader, so point clouds will not draw");
            return null;
        }

        PointMaterial = new Material(shader) { name = "Pointreceiver Points" };
        // nothing has been pushed to a brand new material yet
        AppliedPointSize = float.NaN;

        return PointMaterial;
    }

    void ApplyPointSettings()
    {
        if (PointMaterial == null) return;
        if (PointSize == AppliedPointSize && SizeMode == AppliedSizeMode
            && Shape == AppliedShape && Tint == AppliedTint) return;

        AppliedPointSize = PointSize;
        AppliedSizeMode = SizeMode;
        AppliedShape = Shape;
        AppliedTint = Tint;

        PointMaterial.SetFloat("_PointSize", PointSize);
        PointMaterial.SetFloat("_PositionScale", PositionScale);
        PointMaterial.SetFloat("_PointScaleRange", PointScaleRange);
        PointMaterial.SetColor("_Tint", Tint);

        SetToggle("_Distance", "_DISTANCE_ON", SizeMode == PointSizeMode.WorldUnits);
        SetToggle("_Disk", "_DISK_ON", Shape == PointShape.Disk);
    }

    void SetToggle(string property, string keyword, bool on)
    {
        if (on) 
        {
            PointMaterial.SetFloat(property, 1f);
            PointMaterial.EnableKeyword(keyword);
        }
        else 
        {
            PointMaterial.SetFloat(property, 0f);
            PointMaterial.DisableKeyword(keyword);
        }
    }

    void EnsureCapacity(int pointCount)
    {
        if (CurrentCapacity >= pointCount) return;

        // points per step of buffer growth
        const int CapacityBlock = 8192;
        CurrentCapacity = (pointCount / CapacityBlock + 2) * CapacityBlock;

        if (OutputVertices.IsCreated) OutputVertices.Dispose();
        if (QuadCorners.IsCreated) QuadCorners.Dispose();

        int vertexCapacity = CurrentCapacity * VerticesPerPoint;

        Indices = new int[CurrentCapacity * IndicesPerPoint];
        QuadCorners = new NativeArray<Vector2>(vertexCapacity, Allocator.Persistent, NativeArrayOptions.UninitializedMemory);
        for (int i = 0; i < CurrentCapacity; i++)
        {
            int vertex = i * VerticesPerPoint;
            int index = i * IndicesPerPoint;

            Indices[index + 0] = vertex + 0;
            Indices[index + 1] = vertex + 1;
            Indices[index + 2] = vertex + 2;
            Indices[index + 3] = vertex + 2;
            Indices[index + 4] = vertex + 1;
            Indices[index + 5] = vertex + 3;

            QuadCorners[vertex + 0] = new Vector2(-1f, -1f);
            QuadCorners[vertex + 1] = new Vector2(1f, -1f);
            QuadCorners[vertex + 2] = new Vector2(-1f, 1f);
            QuadCorners[vertex + 3] = new Vector2(1f, 1f);
        }

        OutputVertices = new NativeArray<PointVertex>(vertexCapacity, Allocator.Persistent, NativeArrayOptions.UninitializedMemory);
        if (!MinMax.IsCreated) 
        {
            MinMax = new NativeArray<Vector3>(2, Allocator.Persistent);
        }
        if (!MaxPointScale.IsCreated)
        {
            MaxPointScale = new NativeArray<float>(1, Allocator.Persistent);
        }

        foreach (var channel in PointCloudChannels) 
        {
            ConfigureMesh(channel.Mesh);
        }
    }

    unsafe void UnpackPointCloudIntoMesh(PointCloudFrame frame, Mesh targetMesh)
    {
        int pointCount = frame.PointCount;
        if (pointCount == 0) return;

        var unpackJob = new PointUnpackJob
        {
            Positions = (PackedPosition*)frame.positions.ToPointer(),
            Colors = (uint*)frame.colours.ToPointer(),
            OutVertex = OutputVertices
        };

        var pointScaleAttribute = PointreceiverNative.FindAttribute(ref frame, "point_scale");
        if (pointScaleAttribute != IntPtr.Zero)
        {
            var attribute = *(PointreceiverAttribute*)pointScaleAttribute.ToPointer();
            if (attribute.component_count == 1)
            {
                unpackJob.PointScales = attribute.data.ToPointer();
                unpackJob.PointScaleCount = attribute.ElementCount;
                unpackJob.PointScaleType = attribute.element_type;
                unpackJob.PointScaleStep = attribute.quantisation_step;
                unpackJob.PointScaleOffset = attribute.quantisation_offset;
            }
        }

        var boundsJob = new PointBoundsJob
        {
            Vertex = OutputVertices,
            Count = pointCount,
            MinMax = MinMax,
            MaxPointScale = MaxPointScale
        };
        var handle = boundsJob.Schedule(unpackJob.Schedule(pointCount, 64));
        handle.Complete();

        // quads grow around the centrepoint
        var extent = MinMax[1] - MinMax[0] + Vector3.one * PointSize * Mathf.Max(1f, MaxPointScale[0]);
        var bounds = new Bounds((MinMax[0] + MinMax[1]) * 0.5f, extent);

        ApplyMeshData(targetMesh, pointCount, bounds);
    }

    void ConfigureMesh(Mesh mesh)
    {
        int vertexCapacity = CurrentCapacity * VerticesPerPoint;
        int indexCapacity = CurrentCapacity * IndicesPerPoint;

        mesh.Clear();
        mesh.SetVertexBufferParams(
            vertexCapacity,
            new VertexAttributeDescriptor(VertexAttribute.Position, VertexAttributeFormat.SNorm16, 4, 0),
            new VertexAttributeDescriptor(VertexAttribute.Color, VertexAttributeFormat.UNorm8, 4, 0),
            new VertexAttributeDescriptor(VertexAttribute.TexCoord0, VertexAttributeFormat.Float32, 2, 1)
        );
        mesh.SetVertexBufferData(QuadCorners, 0, 0, vertexCapacity, 1, MeshUpdateFlags.DontRecalculateBounds);

        mesh.SetIndexBufferParams(indexCapacity, IndexFormat.UInt32);
        mesh.SetIndexBufferData(Indices, 0, 0, indexCapacity,
            MeshUpdateFlags.DontRecalculateBounds | MeshUpdateFlags.DontValidateIndices);

        mesh.subMeshCount = 1;
        SetDrawnPointCount(mesh, 0, new Bounds());
    }

    void SetDrawnPointCount(Mesh mesh, int pointCount, Bounds bounds)
    {
        mesh.SetSubMesh(0,
            new SubMeshDescriptor(0, pointCount * IndicesPerPoint, MeshTopology.Triangles)
            {
                firstVertex = 0,
                vertexCount = pointCount * VerticesPerPoint,
                bounds = bounds
            },
            MeshUpdateFlags.DontRecalculateBounds | MeshUpdateFlags.DontValidateIndices);
        mesh.bounds = bounds;
    }

    void ApplyMeshData(Mesh mesh, int pointCount, Bounds bounds)
    {
        int vertexCount = pointCount * VerticesPerPoint;
        mesh.SetVertexBufferData(OutputVertices, 0, 0, vertexCount, 0, MeshUpdateFlags.DontRecalculateBounds);
        SetDrawnPointCount(mesh, pointCount, bounds);
    }

    // only clears a channel we've already seen; a channel whose very first
    // frame is empty has nothing to draw and gets no object
    void ClearChannelMesh(string address)
    {
        var existing = PointCloudChannels.FirstOrDefault(channel => channel.Address == address);
        if (existing == null) return;
        SetDrawnPointCount(existing.Mesh, 0, new Bounds());
    }

    Mesh EnsureOrCreateChannelMesh(string address)
    {
        var existing = PointCloudChannels.FirstOrDefault(channel => channel.Address == address);
        if (existing != null)
            return existing.Mesh;

        var newCloudObject = new GameObject(address);
        newCloudObject.transform.SetParent(transform, worldPositionStays: false);
        newCloudObject.layer = gameObject.layer;

        var newMesh = new Mesh { indexFormat = IndexFormat.UInt32 };
        newMesh.MarkDynamic();
        ConfigureMesh(newMesh);
        newCloudObject.AddComponent<MeshFilter>().sharedMesh = newMesh;

        var meshRenderer = newCloudObject.AddComponent<MeshRenderer>();
        meshRenderer.shadowCastingMode = ShadowCastingMode.Off;
        meshRenderer.receiveShadows = false;
        meshRenderer.sharedMaterial = EnsurePointMaterial();

        PointCloudChannels.Add(new PointCloudChannel
        {
            Address = address,
            GameObject = newCloudObject,
            Mesh = newMesh
        });

        return newMesh;
    }

    [BurstCompile]
    private unsafe struct PointUnpackJob : IJobParallelFor
    {
        [NativeDisableUnsafePtrRestriction] public PackedPosition* Positions;
        [NativeDisableUnsafePtrRestriction] public uint* Colors;

        // one point fills the four vertices of its quad
        [NativeDisableParallelForRestriction] public NativeArray<PointVertex> OutVertex;

        [NativeDisableUnsafePtrRestriction] public void* PointScales;
        public int PointScaleCount;
        public PointreceiverAttributeType PointScaleType;
        public float PointScaleStep;
        public float PointScaleOffset;

        public void Execute(int i)
        {
            var packed = Positions[i];

            var outVertex = new PointVertex
            {
                X = (short)-Mathf.Max(packed.X, short.MinValue + 1),
                Y = packed.Y,
                Z = packed.Z,
                Scale = PackPointScale(i),
                Color = *(Color32*)(Colors + i)
            };

            int vertex = i * VerticesPerPoint;
            for (int corner = 0; corner < VerticesPerPoint; corner++)
            {
                OutVertex[vertex + corner] = outVertex;
            }
        }

        short PackPointScale(int i)
        {
            if (PointScales == null || i >= PointScaleCount) return NoPointScale;

            float rawScale;
            switch (PointScaleType)
            {
                case PointreceiverAttributeType.Float32: rawScale = ((float*)PointScales)[i]; break;
                case PointreceiverAttributeType.UInt8: rawScale = ((byte*)PointScales)[i]; break;
                case PointreceiverAttributeType.UInt16: rawScale = ((ushort*)PointScales)[i]; break;
                case PointreceiverAttributeType.UInt32: rawScale = ((uint*)PointScales)[i]; break;
                case PointreceiverAttributeType.Int8: rawScale = ((sbyte*)PointScales)[i]; break;
                case PointreceiverAttributeType.Int16: rawScale = ((short*)PointScales)[i]; break;
                case PointreceiverAttributeType.Int32: rawScale = ((int*)PointScales)[i]; break;
                default: return NoPointScale;
            }

            float scaleMillimetres = rawScale * PointScaleStep + PointScaleOffset;
            float scaleSteps = scaleMillimetres * PointScaleStepsPerMillimetre + 0.5f;
            return (short)Mathf.Clamp(scaleSteps, 0f, short.MaxValue);
        }
    }

    [BurstCompile]
    private struct PointBoundsJob : IJob
    {
        [ReadOnly] public NativeArray<PointVertex> Vertex;
        public int Count;

        [WriteOnly] public NativeArray<Vector3> MinMax;
        [WriteOnly] public NativeArray<float> MaxPointScale;

        public void Execute()
        {
            var min = new Vector3(float.MaxValue, float.MaxValue, float.MaxValue);
            var max = new Vector3(float.MinValue, float.MinValue, float.MinValue);
            int maxScale = NoPointScale;

            for (int i = 0; i < Count; i++)
            {
                var v = Vertex[i * VerticesPerPoint];
                var p = new Vector3(
                    v.X * MillimetresToMetres,
                    v.Y * MillimetresToMetres,
                    v.Z * MillimetresToMetres);
                min.x = Mathf.Min(min.x, p.x);
                min.y = Mathf.Min(min.y, p.y);
                min.z = Mathf.Min(min.z, p.z);
                max.x = Mathf.Max(max.x, p.x);
                max.y = Mathf.Max(max.y, p.y);
                max.z = Mathf.Max(max.z, p.z);
                maxScale = Mathf.Max(maxScale, v.Scale);
            }

            MinMax[0] = min;
            MinMax[1] = max;
            MaxPointScale[0] = maxScale / PointScaleStepsPerMillimetre;
        }
    }
}
