public static const int kViewOverdraw = 1 << 0;
public static const int kViewMeshlet = 1 << 1;
public static const int kViewBaseColor = 1 << 2;
public static const int kViewNormal = 1 << 3;
public static const int kViewMaterialID = 1 << 4;
public static const int kViewPosition = 1 << 5;

public static const int kViewPTDirect = 1 << 6;

// Per-lobe AOV view flags (Diffuse / Specular)
public static const int kViewAOVDiffuse  = 1 << 7;
public static const int kViewAOVSpecular = 1 << 8;

public static const int kViewEnableRaytracing = 1 << 16;

public static const int kCullFrustum = 1 << 0;
public static const int kCullOcclusion = 1 << 1;
public static const int kCullBackface = 1 << 2;

public static const int kCullStageFirst = 1 << 16;
public static const int kCullStageLate = 1 << 17;

