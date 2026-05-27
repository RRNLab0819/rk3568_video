# Fisheye Mesh Display — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add GPU fisheye mesh undistort rendering to display.c, with FISHEYE_MODE env toggle preserving the existing 2x2 quad baseline.

**Architecture:** New `fisheye_mesh.c/.h` module holds lens table + camera params + mesh generation. display.c calls it during init to build per-camera VBOs, then in disp_draw() either renders via mesh (VBO draw) or quad (existing path). Same NV12→RGB fragment shader reused for both paths.

**Tech Stack:** C + OpenGL ES 2.0 (VBO, GL_TRIANGLES with index buffer) + EGL + Wayland. Cross-compiled via RK3568 SDK aarch64-buildroot-linux-gnu-gcc.

---

### Task 1: Create fisheye_mesh.h — types, lens table, camera params

**Files:**
- Create: `src/fisheye_mesh.h`

- [ ] **Step 1: Write the header file**

```c
/* src/fisheye_mesh.h
 * Fisheye undistort mesh generator — factory "6028" lens model + per-camera params.
 * Generates a UV mesh that maps rectilinear output pixels back to fisheye-source
 * texture coordinates via inverse lens distortion lookup.
 */
#ifndef FISHEYE_MESH_H
#define FISHEYE_MESH_H

#include <GLES2/gl2.h>

/* Lens "6028" distortion table: angle(deg) -> normalized radius (r / FocalLength).
 * Only the monotonic portion (0° – 90°), 181 rows.  After 90° the lens flips. */
#define LENS_6028_ROWS 181

/* Per-camera calibration (from factory calibinfo.lua) */
typedef struct {
    float cx, cy;       /* optical center in pixels */
    float focal;        /* focal length in pixels */
    float scale;        /* scale factor */
    int   src_w, src_h; /* source image size */
} fisheye_cam_t;

/* Generated mesh for one camera */
typedef struct {
    int    num_verts;
    int    num_indices;
    GLuint vbo_pos;     /* vec2 screen-space positions */
    GLuint vbo_tex;     /* vec2 source-image UVs */
    GLuint ibo;         /* unsigned short triangle indices */
} fisheye_mesh_t;

/* Lookup lens distortion: given incident angle (radians), return pixel radius from center.
 * Uses linear interpolation in the lens_6028 table. */
float lens_6028_radius(float angle_rad, float focal, float scale);

/* Generate undistort mesh for one camera tile.
 *   tile_x0, tile_y0, tile_w, tile_h: NDC position of the 2x2 tile
 *   out_w, out_h: pixel resolution of this tile
 *   fov_h: horizontal FOV in degrees
 *   mesh: output mesh with uploaded VBOs
 * Returns 0 on success, -1 on error. */
int  fisheye_mesh_build(fisheye_mesh_t *mesh,
                        const fisheye_cam_t *cam,
                        float tile_x0, float tile_y0,
                        float tile_w,  float tile_h,
                        int out_w, int out_h,
                        float fov_h);

void fisheye_mesh_destroy(fisheye_mesh_t *mesh);

/* Pre-defined camera calibrations (parsed from /userdata/avm/cali/calibinfo.lua) */
extern const fisheye_cam_t g_fisheye_cams[4];

#endif
```

- [ ] **Step 2: Commit**

```bash
git add src/fisheye_mesh.h
git commit -m "feat: add fisheye_mesh.h — types, API, camera params for lens 6028"
```

---

### Task 2: Create fisheye_mesh.c — lens table, camera params, mesh generator

**Files:**
- Create: `src/fisheye_mesh.c`

- [ ] **Step 1: Write the implementation file**

```c
/* src/fisheye_mesh.c */
#include "fisheye_mesh.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Lens "6028" monotonic distortion table (0° – 90°, 0.5° step, 181 rows).
 * Columns: {angle_deg, r_ideal_norm, r_real_norm, error}
 * r_real_norm = measured_pixel_radius / FocalLength_lens (1469.5).
 * Extracted from /oem/birdview/scripts/lens.lua */
static const float lens_6028_table[LENS_6028_ROWS][4] = {
    {0.0f,       0.000000000f, 0.000000000f,  0.00000f},
    {0.5f,       0.012823792f, 0.012824076f, -0.00002f},
    {1.0f,       0.025647834f, 0.025650106f, -0.00009f},
    {1.5f,       0.038472373f, 0.038480044f, -0.00020f},
    {2.0f,       0.051297657f, 0.051315848f, -0.00035f},
    {2.5f,       0.064123934f, 0.064159476f, -0.00055f},
    {3.0f,       0.076951447f, 0.077012897f, -0.00080f},
    {3.5f,       0.089780441f, 0.089878079f, -0.00109f},
    {4.0f,       0.102611156f, 0.102757003f, -0.00142f},
    {4.5f,       0.115443830f, 0.115651655f, -0.00180f},
    {5.0f,       0.128278698f, 0.128564032f, -0.00222f},
    {5.5f,       0.141115991f, 0.141496141f, -0.00269f},
    {6.0f,       0.153955937f, 0.154450002f, -0.00320f},
    {6.5f,       0.166798760f, 0.167427648f, -0.00376f},
    {7.0f,       0.179644680f, 0.180431127f, -0.00436f},
    {7.5f,       0.192493909f, 0.193462503f, -0.00501f},
    {8.0f,       0.205346659f, 0.206523858f, -0.00570f},
    {8.5f,       0.218203133f, 0.219617291f, -0.00644f},
    {9.0f,       0.231063529f, 0.232744922f, -0.00722f},
    {9.5f,       0.243928040f, 0.245908894f, -0.00806f},
    {10.0f,      0.256796853f, 0.259111371f, -0.00893f},
    {10.5f,      0.269670148f, 0.272354541f, -0.00986f},
    {11.0f,      0.282548098f, 0.285640621f, -0.01083f},
    {11.5f,      0.295430871f, 0.298971853f, -0.01184f},
    {12.0f,      0.308318628f, 0.312350508f, -0.01291f},
    {12.5f,      0.321211520f, 0.325778889f, -0.01402f},
    {13.0f,      0.334109695f, 0.339259331f, -0.01518f},
    {13.5f,      0.347013290f, 0.352794201f, -0.01639f},
    {14.0f,      0.359922438f, 0.366385906f, -0.01764f},
    {14.5f,      0.372837261f, 0.380036886f, -0.01894f},
    {15.0f,      0.385757877f, 0.393749625f, -0.02030f},
    {15.5f,      0.398684394f, 0.407526644f, -0.02170f},
    {16.0f,      0.411616913f, 0.421370511f, -0.02315f},
    {16.5f,      0.424555527f, 0.435283837f, -0.02465f},
    {17.0f,      0.437500322f, 0.449269281f, -0.02620f},
    {17.5f,      0.450451376f, 0.463329554f, -0.02779f},
    {18.0f,      0.463408759f, 0.477467416f, -0.02944f},
    {18.5f,      0.476372533f, 0.491685682f, -0.03114f},
    {19.0f,      0.489342754f, 0.505987226f, -0.03290f},
    {19.5f,      0.502319467f, 0.520374978f, -0.03470f},
    {20.0f,      0.515302714f, 0.534851932f, -0.03655f},
    {20.5f,      0.528292525f, 0.549421146f, -0.03846f},
    {21.0f,      0.541288926f, 0.564085745f, -0.04041f},
    {21.5f,      0.554291933f, 0.578848925f, -0.04242f},
    {22.0f,      0.567301556f, 0.593713955f, -0.04449f},
    {22.5f,      0.580317798f, 0.608684181f, -0.04660f},
    {23.0f,      0.593340653f, 0.623763028f, -0.04877f},
    {23.5f,      0.606370111f, 0.638954005f, -0.05100f},
    {24.0f,      0.619406152f, 0.654260706f, -0.05327f},
    {24.5f,      0.632448749f, 0.669686818f, -0.05561f},
    {25.0f,      0.645497871f, 0.685236122f, -0.05799f},
    {25.5f,      0.658553478f, 0.700912495f, -0.06043f},
    {26.0f,      0.671615522f, 0.716719920f, -0.06293f},
    {26.5f,      0.684683951f, 0.732662485f, -0.06549f},
    {27.0f,      0.697758705f, 0.748744390f, -0.06809f},
    {27.5f,      0.710839717f, 0.764969952f, -0.07076f},
    {28.0f,      0.723926914f, 0.781343610f, -0.07348f},
    {28.5f,      0.737020217f, 0.797869929f, -0.07627f},
    {29.0f,      0.750119539f, 0.814553607f, -0.07910f},
    {29.5f,      0.763224787f, 0.831399480f, -0.08200f},
    {30.0f,      0.776335861f, 0.848412529f, -0.08495f},
    {30.5f,      0.789452657f, 0.865597885f, -0.08797f},
    {31.0f,      0.802575061f, 0.882960837f, -0.09104f},
    {31.5f,      0.815702955f, 0.900506840f, -0.09417f},
    {32.0f,      0.828836212f, 0.918241517f, -0.09737f},
    {32.5f,      0.841974699f, 0.936170674f, -0.10062f},
    {33.0f,      0.855118277f, 0.954300306f, -0.10393f},
    {33.5f,      0.868266800f, 0.972636600f, -0.10731f},
    {34.0f,      0.881420114f, 0.991185952f, -0.11074f},
    {34.5f,      0.894578058f, 1.009954974f, -0.11424f},
    {35.0f,      0.907740463f, 1.028950500f, -0.11780f},
    {35.5f,      0.920907155f, 1.048179602f, -0.12142f},
    {36.0f,      0.934077949f, 1.067649599f, -0.12511f},
    {36.5f,      0.947252653f, 1.087368068f, -0.12886f},
    {37.0f,      0.960431069f, 1.107342858f, -0.13267f},
    {37.5f,      0.973612987f, 1.127582102f, -0.13655f},
    {38.0f,      0.986798190f, 1.148094232f, -0.14049f},
    {38.5f,      0.999986453f, 1.168887993f, -0.14450f},
    {39.0f,      1.013177540f, 1.189972459f, -0.14857f},
    {39.5f,      1.026371206f, 1.211357048f, -0.15271f},
    {40.0f,      1.039567197f, 1.233051542f, -0.15692f},
    {40.5f,      1.052765246f, 1.255066106f, -0.16119f},
    {41.0f,      1.065965078f, 1.277411302f, -0.16553f},
    {41.5f,      1.079166405f, 1.300098119f, -0.16993f},
    {42.0f,      1.092368929f, 1.323137985f, -0.17441f},
    {42.5f,      1.105572339f, 1.346542801f, -0.17895f},
    {43.0f,      1.118776312f, 1.370324956f, -0.18357f},
    {43.5f,      1.131980511f, 1.394497363f, -0.18825f},
    {44.0f,      1.145184587f, 1.419073479f, -0.19301f},
    {44.5f,      1.158388177f, 1.444067344f, -0.19783f},
    {45.0f,      1.171590903f, 1.469493605f, -0.20272f},
    {45.5f,      1.184792373f, 1.495367557f, -0.20769f},
    {46.0f,      1.197992177f, 1.521705174f, -0.21273f},
    {46.5f,      1.211189893f, 1.548523156f, -0.21784f},
    {47.0f,      1.224385080f, 1.575838962f, -0.22303f},
    {47.5f,      1.237577278f, 1.603670864f, -0.22828f},
    {48.0f,      1.250766015f, 1.632037989f, -0.23362f},
    {48.5f,      1.263950794f, 1.660960373f, -0.23902f},
    {49.0f,      1.277131103f, 1.690459018f, -0.24451f},
    {49.5f,      1.290306411f, 1.720555950f, -0.25006f},
    {50.0f,      1.303476164f, 1.751274284f, -0.25570f},
    {50.5f,      1.316642120f, 1.782638290f, -0.26141f},
    {51.0f,      1.329801126f, 1.814673475f, -0.26720f},
    {51.5f,      1.342952550f, 1.847406654f, -0.27306f},
    {52.0f,      1.356095736f, 1.880866044f, -0.27900f},
    {52.5f,      1.369230005f, 1.915081352f, -0.28503f},
    {53.0f,      1.382354658f, 1.950083879f, -0.29113f},
    {53.5f,      1.395468969f, 1.985906631f, -0.29731f},
    {54.0f,      1.408572190f, 2.022584431f, -0.30358f},
    {54.5f,      1.421663546f, 2.060154054f, -0.30992f},
    {55.0f,      1.434742238f, 2.098654363f, -0.31635f},
    {55.5f,      1.447807442f, 2.138126463f, -0.32286f},
    {56.0f,      1.460858306f, 2.178613863f, -0.32946f},
    {56.5f,      1.473893952f, 2.220162656f, -0.33613f},
    {57.0f,      1.486913475f, 2.262821717f, -0.34289f},
    {57.5f,      1.499915940f, 2.306642918f, -0.34974f},
    {58.0f,      1.512900388f, 2.351681357f, -0.35667f},
    {58.5f,      1.525865827f, 2.397995619f, -0.36369f},
    {59.0f,      1.538811239f, 2.445648057f, -0.37080f},
    {59.5f,      1.551735575f, 2.494705098f, -0.37799f},
    {60.0f,      1.564637757f, 2.545237586f, -0.38527f},
    {60.5f,      1.577517602f, 2.597321154f, -0.39264f},
    {61.0f,      1.590375057f, 2.651036640f, -0.40009f},
    {61.5f,      1.603200090f, 2.706480751f, -0.40814f},
    {62.0f,      1.616004718f, 2.763710879f, -0.41611f},
    {62.5f,      1.628774765f, 2.822859099f, -0.42461f},
    {63.0f,      1.641521275f, 2.884035931f, -0.43318f},
    {63.5f,      1.654239654f, 2.947379510f, -0.44199f},
    {64.0f,      1.666926265f, 3.013044299f, -0.45111f},
    {64.5f,      1.679575086f, 3.081163584f, -0.46067f},
    {65.0f,      1.692198396f, 3.151831884f, -0.47007f},
    {65.5f,      1.704781532f, 3.225380241f, -0.48003f},
    {66.0f,      1.717338204f, 3.301907122f, -0.48990f},
    {66.5f,      1.729836702f, 3.381787538f, -0.50069f},
    {67.0f,      1.742302895f, 3.465308903f, -0.51163f},
    {67.5f,      1.754722357f, 3.552837371f, -0.52298f},
    {68.0f,      1.767105103f, 3.644653916f, -0.53453f},
    {68.5f,      1.779428124f, 3.741281025f, -0.54661f},
    {69.0f,      1.791712403f, 3.843083620f, -0.55896f},
    {69.5f,      1.803922534f, 3.950646877f, -0.57206f},
    {70.0f,      1.816077828f, 4.064623833f, -0.58556f},
    {70.5f,      1.828175068f, 4.185658390f, -0.59955f},
    {71.0f,      1.840194941f, 4.314507484f, -0.61433f},
    {71.5f,      1.852138519f, 4.452461004f, -0.62985f},
    {72.0f,      1.864009142f, 4.600624323f, -0.64614f},
    {72.5f,      1.875793576f, 4.760204135f, -0.66341f},
    {73.0f,      1.887490034f, 4.932394743f, -0.68189f},
    {73.5f,      1.899077773f, 5.119271993f, -0.70183f},
    {74.0f,      1.910595536f, 5.323178659f, -0.72309f},
    {74.5f,      1.922015786f, 5.546142816f, -0.74593f},
    {75.0f,      1.933349490f, 5.791478096f, -0.77063f},
    {75.5f,      1.944578052f, 6.062361233f, -0.79748f},
    {76.0f,      1.955714464f, 6.361119813f, -0.82616f},
    {76.5f,      1.966714382f, 6.694724322f, -0.85766f},
    {77.0f,      1.977636218f, 7.061966896f, -0.89116f},
    {77.5f,      1.988419294f, 7.473524570f, -0.92780f},
    {78.0f,      1.999085665f, 7.933609962f, -0.96709f},
    {78.5f,      2.009603262f, 8.450008392f, -1.00985f},
    {79.0f,      2.019998074f, 9.034533978f, -1.05653f},
    {79.5f,      2.030244112f, 9.697914124f, -1.10778f},
    {80.0f,      2.040333271f, 10.447729111f, -1.16387f},
    {80.5f,      2.050270081f, 11.283519745f, -1.22474f},
    {81.0f,      2.060044050f, 12.177201271f, -1.28995f},
    {81.5f,      2.069663286f, 12.976125717f, -1.35833f},
    {82.0f,      2.079117537f, 13.068449020f, -1.42404f},
    {82.5f,      2.088429451f, 11.786231041f, -1.47595f},
    {83.0f,      2.097583294f, 10.752637863f, -1.51602f},
    {83.5f,      2.106566429f, 9.897455215f, -1.54278f},
    {84.0f,      2.115380049f, 9.211202621f, -1.55707f},
    {84.5f,      2.124024630f, 8.611497879f, -1.56032f},
    {85.0f,      2.132513046f, 8.066040039f, -1.55416f},
    {85.5f,      2.140838385f, 7.598874092f, -1.54027f},
    {86.0f,      2.149002790f, 7.193012238f, -1.52077f},
    {86.5f,      2.157001257f, 6.829543114f, -1.49658f},
    {87.0f,      2.164841652f, 6.500413895f, -1.46886f},
    {87.5f,      2.172517061f, 6.197177887f, -1.43872f},
    {88.0f,      2.180035591f, 5.919258118f, -1.40650f},
    {88.5f,      2.187407017f, 5.663145065f, -1.37281f},
    {89.0f,      2.194631100f, 5.423220158f, -1.33832f},
    {89.5f,      2.201697111f, 5.198241711f, -1.30295f},
    {90.0f,      2.208615303f, 4.985487461f, -1.26702f},
};

/* Camera calibrations from /userdata/avm/cali/calibinfo.lua */
const fisheye_cam_t g_fisheye_cams[4] = {
    { .cx = 946.4f, .cy = 553.0f, .focal = 1449.5f, .scale = 1.005f, .src_w = 1920, .src_h = 1080 },
    { .cx = 919.6f, .cy = 551.2f, .focal = 1449.5f, .scale = 1.021f, .src_w = 1920, .src_h = 1080 },
    { .cx = 1008.9f, .cy = 588.9f, .focal = 1449.5f, .scale = 0.987f, .src_w = 1920, .src_h = 1080 },
    { .cx = 934.2f, .cy = 542.1f, .focal = 1449.5f, .scale = 1.032f, .src_w = 1920, .src_h = 1080 },
};

/* ------------------------------------------------------------------ */
/* Lens lookup                                                        */
/* ------------------------------------------------------------------ */

float lens_6028_radius(float angle_rad, float focal, float scale)
{
    float angle_deg = (float)(angle_rad * 180.0 / M_PI);
    if (angle_deg <= 0.0f) return 0.0f;
    if (angle_deg >= 90.0f)
        angle_deg = 90.0f;

    /* Binary search in 0.5°-step table */
    int lo = 0, hi = LENS_6028_ROWS - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (lens_6028_table[mid][0] <= angle_deg)
            lo = mid;
        else
            hi = mid - 1;
    }
    int i0 = lo;
    int i1 = (i0 + 1 < LENS_6028_ROWS) ? i0 + 1 : i0;

    float t = 0.0f;
    float d = lens_6028_table[i1][0] - lens_6028_table[i0][0];
    if (d > 0.0f)
        t = (angle_deg - lens_6028_table[i0][0]) / d;

    float r_norm = lens_6028_table[i0][2] * (1.0f - t)
                 + lens_6028_table[i1][2] * t;

    return r_norm * focal * scale;
}

/* ------------------------------------------------------------------ */
/* Mesh generator                                                     */
/* ------------------------------------------------------------------ */

#define MESH_DIV 48

int fisheye_mesh_build(fisheye_mesh_t *mesh,
                       const fisheye_cam_t *cam,
                       float tile_x0, float tile_y0,
                       float tile_w,  float tile_h,
                       int out_w, int out_h,
                       float fov_h)
{
    int gw = MESH_DIV, gh = MESH_DIV;
    int nv = (gw + 1) * (gh + 1);
    int ni = gw * gh * 6;

    float *pos = (float *)malloc(nv * 2 * sizeof(float));
    float *tex = (float *)malloc(nv * 2 * sizeof(float));
    unsigned short *idx = (unsigned short *)malloc(ni * sizeof(unsigned short));

    if (!pos || !tex || !idx) {
        free(pos); free(tex); free(idx);
        return -1;
    }

    float fov_v = fov_h * (float)out_h / (float)out_w;
    float fov_h_rad = fov_h * (float)(M_PI / 180.0);
    float fov_v_rad = fov_v * (float)(M_PI / 180.0);

    int valid = 0;

    for (int j = 0; j <= gh; j++) {
        for (int i = 0; i <= gw; i++) {
            int vi = j * (gw + 1) + i;
            float u = (float)i / (float)gw;
            float v = (float)j / (float)gh;

            /* Screen position in NDC for this tile */
            pos[vi * 2 + 0] = tile_x0 + u * tile_w;
            pos[vi * 2 + 1] = tile_y0 + v * tile_h;

            /* Ray angles from output pixel (rectilinear / pinhole model) */
            float theta = (u - 0.5f) * fov_h_rad;
            float phi   = (v - 0.5f) * fov_v_rad;
            float cos_theta = cosf(theta);

            /* Pinhole projection to ideal sensor coords */
            float dx = cam->focal * tanf(theta);
            float dy = cam->focal * tanf(phi) / cos_theta;

            /* Incident angle from optical axis */
            float r_ideal = sqrtf(dx * dx + dy * dy);
            float inc_angle = atan2f(r_ideal, cam->focal);

            /* Use lens table to get real (distorted) radius */
            float r_real = lens_6028_radius(inc_angle, cam->focal, cam->scale);

            /* Source UV in the fisheye image */
            float sx, sy;
            if (r_ideal > 1e-6f) {
                sx = cam->cx + dx * (r_real / r_ideal);
                sy = cam->cy + dy * (r_real / r_ideal);
            } else {
                sx = cam->cx;
                sy = cam->cy;
            }

            tex[vi * 2 + 0] = sx / (float)cam->src_w;
            tex[vi * 2 + 1] = sy / (float)cam->src_h;

            if (sx >= 0.0f && sx < (float)cam->src_w &&
                sy >= 0.0f && sy < (float)cam->src_h)
                valid++;
        }
    }

    /* Generate triangle indices */
    int ii = 0;
    for (int j = 0; j < gh; j++) {
        for (int i = 0; i < gw; i++) {
            int a = j * (gw + 1) + i;
            int b = a + 1;
            int c = a + (gw + 1);
            int d = c + 1;
            idx[ii++] = (unsigned short)a;
            idx[ii++] = (unsigned short)b;
            idx[ii++] = (unsigned short)d;
            idx[ii++] = (unsigned short)a;
            idx[ii++] = (unsigned short)d;
            idx[ii++] = (unsigned short)c;
        }
    }

    /* Upload to GPU */
    glGenBuffers(1, &mesh->vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, nv * 2 * sizeof(float), pos, GL_STATIC_DRAW);

    glGenBuffers(1, &mesh->vbo_tex);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo_tex);
    glBufferData(GL_ARRAY_BUFFER, nv * 2 * sizeof(float), tex, GL_STATIC_DRAW);

    glGenBuffers(1, &mesh->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, ni * sizeof(unsigned short), idx, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    mesh->num_verts   = nv;
    mesh->num_indices = ni;

    float ratio = (float)valid * 100.0f / (float)nv;
    printf("[mesh] cam: cx=%.1f cy=%.1f focal=%.1f scale=%.3f  "
           "verts=%d idx=%d valid_uv=%.1f%%\n",
           cam->cx, cam->cy, cam->focal, cam->scale,
           nv, ni, ratio);

    free(pos); free(tex); free(idx);
    return 0;
}

void fisheye_mesh_destroy(fisheye_mesh_t *mesh)
{
    if (mesh->vbo_pos) glDeleteBuffers(1, &mesh->vbo_pos);
    if (mesh->vbo_tex) glDeleteBuffers(1, &mesh->vbo_tex);
    if (mesh->ibo)     glDeleteBuffers(1, &mesh->ibo);
    memset(mesh, 0, sizeof(*mesh));
}
```

- [ ] **Step 2: Commit**

```bash
git add src/fisheye_mesh.c
git commit -m "feat: add fisheye_mesh.c — lens 6028 table, mesh generator"
```

---

### Task 3: Modify display.c — add fisheye mesh rendering path

**Files:**
- Modify: `src/display.c`
- Modify: `src/shader_yuv.h` (add mesh vertex shader)

- [ ] **Step 1: Add mesh vertex shader to shader_yuv.h**

```c
/* Add after frag_src in src/shader_yuv.h: */

/* Vertex shader for fisheye mesh mode — position and texcoord from VBOs */
static const char mesh_vert_src[] =
    "#version 100\n"
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_tex;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "  v_tex = a_tex;\n"
    "}\n";
```

- [ ] **Step 2: Add mesh fields to display_s struct**

```c
/* Add after has_frame[4] field in struct display_s: */
    /* Fisheye mesh mode */
    bool        fisheye_mode;
    GLuint      mesh_prog;       /* shader program for mesh path */
    GLuint      mesh_vbo_pos[4]; /* per-camera position VBO */
    GLuint      mesh_vbo_tex[4]; /* per-camera texcoord VBO */
    GLuint      mesh_ibo[4];     /* per-camera index buffer */
    int         mesh_nidx[4];    /* per-camera index count */
```

- [ ] **Step 3: Add include and modify disp_open()**

```c
/* Add at top of display.c, after existing includes: */
#include "fisheye_mesh.h"

/* In disp_open(), after eglMakeCurrent and shader compilation: */

    /* ---- Fisheye mesh mode (env toggle) ---- */
    const char *fm = getenv("FISHEYE_MODE");
    d->fisheye_mode = (fm && fm[0] == '1');

    if (d->fisheye_mode) {
        /* Compile mesh shader */
        d->mesh_prog = glCreateProgram();
        { GLuint v = compile_shader(GL_VERTEX_SHADER, mesh_vert_src);
          /* Reuse existing YUV fragment shader */
          GLuint f = compile_shader(GL_FRAGMENT_SHADER, frag_src);
          glAttachShader(d->mesh_prog, v);
          glAttachShader(d->mesh_prog, f);
          glLinkProgram(d->mesh_prog);
          glDeleteShader(v); glDeleteShader(f); }

        /* Generate mesh per camera */
        int cols = (d->n_cams <= 2) ? d->n_cams : 2;
        int rows = (d->n_cams <= 2) ? 1 : 2;
        float qw = 2.0f / cols;
        float qh = 2.0f / rows;

        for (int i = 0; i < d->n_cams; i++) {
            int col = i % cols, row = i / cols;
            float x0 = -1.0f + col * qw;
            float y0 =  1.0f - (row + 1) * qh;

            fisheye_mesh_t m;
            if (fisheye_mesh_build(&m, &g_fisheye_cams[i],
                                   x0, y0, qw, qh,
                                   960, 540, 48.0f) == 0) {
                d->mesh_vbo_pos[i] = m.vbo_pos;
                d->mesh_vbo_tex[i] = m.vbo_tex;
                d->mesh_ibo[i]     = m.ibo;
                d->mesh_nidx[i]    = m.num_indices;
            }
        }
        printf("[display] fisheye mesh mode ON (4 cameras)\n");
    }
```

- [ ] **Step 4: Modify disp_draw() to branch on fisheye_mode**

Replace the current quad rendering loop in disp_draw() with a branch:

```c
    if (d->fisheye_mode) {
        /* ---- Fisheye mesh rendering path ---- */
        glUseProgram(d->mesh_prog);
        GLint u_texY  = glGetUniformLocation(d->mesh_prog, "u_texY");
        GLint u_texUV = glGetUniformLocation(d->mesh_prog, "u_texUV");
        GLint loc_pos = glGetAttribLocation(d->mesh_prog, "a_pos");
        GLint loc_tex = glGetAttribLocation(d->mesh_prog, "a_tex");

        for (int i = 0; i < d->n_cams; i++) {
            if (!d->has_frame[i]) continue;

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, d->texY[i]);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, d->texUV[i]);
            glUniform1i(u_texY, 0);
            glUniform1i(u_texUV, 1);

            glBindBuffer(GL_ARRAY_BUFFER, d->mesh_vbo_pos[i]);
            glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
            glEnableVertexAttribArray(loc_pos);

            glBindBuffer(GL_ARRAY_BUFFER, d->mesh_vbo_tex[i]);
            glVertexAttribPointer(loc_tex, 2, GL_FLOAT, GL_FALSE, 0, 0);
            glEnableVertexAttribArray(loc_tex);

            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d->mesh_ibo[i]);
            glDrawElements(GL_TRIANGLES, d->mesh_nidx[i],
                           GL_UNSIGNED_SHORT, 0);

            glDisableVertexAttribArray(loc_pos);
            glDisableVertexAttribArray(loc_tex);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    } else {
        /* ---- Original quad path (unchanged) ---- */
        /* ... existing quad rendering code ... */
    }
```

- [ ] **Step 5: Modify disp_close() to clean up mesh resources**

```c
/* In disp_close(), after glDeleteProgram(d->program): */
    if (d->fisheye_mode) {
        glDeleteProgram(d->mesh_prog);
        for (int i = 0; i < d->n_cams; i++) {
            if (d->mesh_vbo_pos[i]) glDeleteBuffers(1, &d->mesh_vbo_pos[i]);
            if (d->mesh_vbo_tex[i]) glDeleteBuffers(1, &d->mesh_vbo_tex[i]);
            if (d->mesh_ibo[i])     glDeleteBuffers(1, &d->mesh_ibo[i]);
        }
    }
```

- [ ] **Step 6: Build and check for compilation errors**

Run:
```bash
cd /home/rrn/rk3568-camera && source /home/rrn/3568/3568_sdk/environment-setup && make clean && make 2>&1
```
Expected: `=== Build OK: rk3568_camera ===`

- [ ] **Step 7: Commit**

```bash
git add src/display.c src/shader_yuv.h src/fisheye_mesh.h src/fisheye_mesh.c Makefile
git commit -m "feat: add GPU fisheye mesh undistort mode (FISHEYE_MODE=1)"
```

---

### Task 4: Deploy and verify

**Files:**
- Deploy: `rk3568_camera`

- [ ] **Step 1: Push to board**

```bash
adb push /home/rrn/rk3568-camera/rk3568_camera /userdata/rk3568_camera
adb shell chmod +x /userdata/rk3568_camera
```

- [ ] **Step 2: Test baseline (FISHEYE_MODE unset)**

Run:
```bash
adb shell "cd /userdata && LD_LIBRARY_PATH=/usr/lib timeout 5 ./rk3568_camera --no-enc 2>&1" | head -20
```
Expected:
```
[display] Wayland+EGL init 1920x1080, 4 cameras
```
No "fisheye mesh" messages. Display shows normal 2x2 quad.

- [ ] **Step 3: Test fisheye mode**

Run:
```bash
adb shell "cd /userdata && FISHEYE_MODE=1 LD_LIBRARY_PATH=/usr/lib timeout 5 ./rk3568_camera --no-enc 2>&1" | head -30
```
Expected output includes:
```
[mesh] cam: cx=946.4 cy=553.0 focal=1449.5 scale=1.005  verts=2401 idx=13824 valid_uv=XX%
[mesh] cam: cx=919.6 cy=551.2 focal=1449.5 scale=1.021  verts=2401 idx=13824 valid_uv=XX%
[mesh] cam: cx=1008.9 cy=588.9 focal=1449.5 scale=0.987  verts=2401 idx=13824 valid_uv=XX%
[mesh] cam: cx=934.2 cy=542.1 focal=1449.5 scale=1.032  verts=2401 idx=13824 valid_uv=XX%
[display] fisheye mesh mode ON (4 cameras)
```

- [ ] **Step 4: Verify no crash, FPS normal**

Run:
```bash
adb shell "cd /userdata && FISHEYE_MODE=1 LD_LIBRARY_PATH=/usr/lib timeout 10 ./rk3568_camera --no-enc 2>&1" | grep -E "mesh|display|sig|fps"
```
Expected: No crash, no signal errors, display init OK.

---

## Unchanged files checklist

| File | Status |
|------|--------|
| `src/capture.c` / `capture.h` | NOT modified |
| `src/inference.cc` / `inference.h` | NOT modified |
| `src/postprocess.cc` / `postprocess.h` | NOT modified |
| `src/encoder.c` / `encoder.h` | NOT modified |
| `src/pipeline.c` / `pipeline.h` | NOT modified |
| `src/main.c` | NOT modified |
| `src/frame.h` | NOT modified |
| `src/display.h` | NOT modified (mesh fields go in struct display_s in .c) |
| `config.ini` | NOT modified |
| `start.sh` | NOT modified |

## Rollback

```bash
# Unset FISHEYE_MODE → back to quad baseline
cd /userdata && LD_LIBRARY_PATH=/usr/lib ./rk3568_camera
```
