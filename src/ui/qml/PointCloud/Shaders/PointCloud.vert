VARYING flat vec4 vColor;

void MAIN()
{
    int xy   = floatBitsToInt(VERTEX.x);
    int zp   = floatBitsToInt(VERTEX.y);
    uint rgba = floatBitsToUint(VERTEX.z);

    float px = float(int(xy << 16) >> 16);
    float py = float(xy >> 16);
    float pz = float(int(zp << 16) >> 16);

    vec3 pos = vec3(px, py, pz) * 0.1;

    vec3 srgb = vec3(
        float(rgba & 0xFFu),
        float((rgba >> 8u) & 0xFFu),
        float((rgba >> 16u) & 0xFFu)
    ) / 255.0;
    vColor = vec4(srgb, 1.0);

    POSITION = MODELVIEWPROJECTION_MATRIX * vec4(pos, 1.0);

    float pointSize = uPointSize;
    if (uHasPointScale) {
        pointSize = UV0.y * 0.2;
    }

    float viewportHeight = uViewportHeight > 1.0 ? uViewportHeight : 1080.0;
    float pixelsPerUnit = abs(PROJECTION_MATRIX[1][1]) * viewportHeight * 0.5;
    float diameter = pointSize * pixelsPerUnit / max(POSITION.w, 0.001);

    POINT_SIZE = pointSize > 0.0
        ? clamp(diameter, uMinPointPixels, uMaxPointPixels)
        : 0.0;
    if (pointSize <= 0.0) POSITION = vec4(0.0, 0.0, -2.0, 1.0);
}
