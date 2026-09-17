// Billboarded quad point renderer for Pointreceiver meshes.
//
// Each point is expanded into a camera facing quad entirely in the vertex
// shader, so it needs nothing beyond a plain vertex stage. The two usual ways
// to size a point cannot be relied on here: the PSIZE semantic is clamped to a
// single pixel by most mobile drivers, and a geometry stage is unsupported or
// very slow on mobile GLES and Vulkan.
//
// Colour handling and tinting follow Pcx (https://github.com/keijiro/Pcx).

Shader "Point Cloud/Point Quad"
{
    Properties
    {
        _Tint("Tint", Color) = (0.5, 0.5, 0.5, 1)
        _PointSize("Point Size", Float) = 0.01
        [Toggle] _Distance("Apply Distance", Float) = 1
        [Toggle] _Disk("Round Points", Float) = 0
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" }

        // quads are built in view space, so winding depends on the host
        // transform; never cull them
        Cull Off

        Pass
        {
            CGPROGRAM

            #pragma vertex Vertex
            #pragma fragment Fragment

            #pragma multi_compile_fog
            #pragma multi_compile_instancing
            #pragma multi_compile _ UNITY_COLORSPACE_GAMMA
            #pragma multi_compile _ _DISTANCE_ON
            #pragma multi_compile _ _DISK_ON

            #include "UnityCG.cginc"

            struct Attributes
            {
                // xyz is the position over the normalised range. w is the
                // point scale over its own range, negative when there is none
                float4 position : POSITION;
                half4 color : COLOR;
                // which corner of its quad this vertex is, as -1/+1 in x and y
                float2 corner : TEXCOORD0;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            struct Varyings
            {
                float4 position : SV_Position;
                half3 color : COLOR;
                float2 corner : TEXCOORD0;
                UNITY_FOG_COORDS(1)
                UNITY_VERTEX_OUTPUT_STEREO
            };

            half4 _Tint;
            half _PointSize;
            // set by PointreceiverMeshHost, which owns the position encoding.
            // deliberately not a material property: it describes the incoming
            // data rather than anything there is a choice about
            float _PositionScale;
            float _PointScaleRange;

            Varyings Vertex(Attributes input)
            {
                Varyings o;
                UNITY_SETUP_INSTANCE_ID(input);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);

                half3 col = input.color.rgb;
            #ifdef UNITY_COLORSPACE_GAMMA
                col *= _Tint.rgb * 2;
            #else
                col *= LinearToGammaSpace(_Tint.rgb) * 2;
                col = GammaToLinearSpace(col);
            #endif

                float2 corner = input.corner;

                float pointScale = input.position.w * _PointScaleRange;
                float pointSize = _PointSize * lerp(1.0, pointScale, step(0.0, input.position.w));

                float3 viewPos = UnityObjectToViewPos(input.position.xyz * _PositionScale);

            #ifdef _DISTANCE_ON
                // pointSize is a width in world units, so offsetting in view
                // space keeps a point the same size in the scene, shrinking
                // with distance
                viewPos.xy += corner * pointSize * 0.5;
                o.position = mul(UNITY_MATRIX_P, float4(viewPos, 1));
            #else
                // pointSize is a width in pixels, so offsetting after the
                // projection keeps a point the same size on screen at any depth
                o.position = mul(UNITY_MATRIX_P, float4(viewPos, 1));
                o.position.xy += corner * pointSize * o.position.w / _ScreenParams.xy;
            #endif

                o.color = col;
                o.corner = corner;
                UNITY_TRANSFER_FOG(o, o.position);
                return o;
            }

            half4 Fragment(Varyings input) : SV_Target
            {
            #ifdef _DISK_ON
                // trim the quad to its inscribed circle
                clip(1 - dot(input.corner, input.corner));
            #endif
                half4 c = half4(input.color, _Tint.a);
                UNITY_APPLY_FOG(input.fogCoord, c);
                return c;
            }

            ENDCG
        }
    }
}
