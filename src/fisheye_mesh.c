/* src/fisheye_mesh.c */
#include "fisheye_mesh.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define LENS_6028_ROWS 181

/* Lens "6028" monotonic distortion table (0deg - 90deg, 0.5deg step, 181 rows).
 * Columns: {angle_deg, r_ideal_norm, r_real_norm, error}
 * r_real_norm = measured_pixel_radius / FocalLength_lens (1469.5).
 * Extracted from /oem/birdview/scripts/lens.lua */
static const float lens_6028_table[LENS_6028_ROWS][4] = {
    {0.0f, 0.000000000f, 0.000000000f, 0.000000000f},
    {0.5f, 0.012823792f, 0.012824076f, -0.000020000f},
    {1.0f, 0.025647834f, 0.025650106f, -0.000090000f},
    {1.5f, 0.038472373f, 0.038480044f, -0.000200000f},
    {2.0f, 0.051297657f, 0.051315848f, -0.000350000f},
    {2.5f, 0.064123934f, 0.064159476f, -0.000550000f},
    {3.0f, 0.076951447f, 0.077012897f, -0.000800000f},
    {3.5f, 0.089780441f, 0.089878079f, -0.001090000f},
    {4.0f, 0.102611156f, 0.102757003f, -0.001420000f},
    {4.5f, 0.115443830f, 0.115651655f, -0.001800000f},
    {5.0f, 0.128278698f, 0.128564032f, -0.002220000f},
    {5.5f, 0.141115991f, 0.141496141f, -0.002690000f},
    {6.0f, 0.153955937f, 0.154450002f, -0.003200000f},
    {6.5f, 0.166798760f, 0.167427648f, -0.003760000f},
    {7.0f, 0.179644680f, 0.180431127f, -0.004360000f},
    {7.5f, 0.192493909f, 0.193462503f, -0.005010000f},
    {8.0f, 0.205346659f, 0.206523858f, -0.005700000f},
    {8.5f, 0.218203133f, 0.219617291f, -0.006440000f},
    {9.0f, 0.231063529f, 0.232744922f, -0.007220000f},
    {9.5f, 0.243928040f, 0.245908894f, -0.008060000f},
    {10.0f, 0.256796853f, 0.259111371f, -0.008930000f},
    {10.5f, 0.269670148f, 0.272354541f, -0.009860000f},
    {11.0f, 0.282548098f, 0.285640621f, -0.010830000f},
    {11.5f, 0.295430871f, 0.298971853f, -0.011840000f},
    {12.0f, 0.308318628f, 0.312350508f, -0.012910000f},
    {12.5f, 0.321211520f, 0.325778889f, -0.014020000f},
    {13.0f, 0.334109695f, 0.339259331f, -0.015180000f},
    {13.5f, 0.347013290f, 0.352794201f, -0.016390000f},
    {14.0f, 0.359922438f, 0.366385906f, -0.017640000f},
    {14.5f, 0.372837261f, 0.380036886f, -0.018940000f},
    {15.0f, 0.385757877f, 0.393749625f, -0.020300000f},
    {15.5f, 0.398684394f, 0.407526644f, -0.021700000f},
    {16.0f, 0.411616913f, 0.421370511f, -0.023150000f},
    {16.5f, 0.424555527f, 0.435283837f, -0.024650000f},
    {17.0f, 0.437500322f, 0.449269281f, -0.026200000f},
    {17.5f, 0.450451376f, 0.463329554f, -0.027790000f},
    {18.0f, 0.463408759f, 0.477467416f, -0.029440000f},
    {18.5f, 0.476372533f, 0.491685682f, -0.031140000f},
    {19.0f, 0.489342754f, 0.505987226f, -0.032900000f},
    {19.5f, 0.502319467f, 0.520374978f, -0.034700000f},
    {20.0f, 0.515302714f, 0.534851932f, -0.036550000f},
    {20.5f, 0.528292525f, 0.549421146f, -0.038460000f},
    {21.0f, 0.541288926f, 0.564085745f, -0.040410000f},
    {21.5f, 0.554291933f, 0.578848925f, -0.042420000f},
    {22.0f, 0.567301556f, 0.593713955f, -0.044490000f},
    {22.5f, 0.580317798f, 0.608684181f, -0.046600000f},
    {23.0f, 0.593340653f, 0.623763028f, -0.048770000f},
    {23.5f, 0.606370111f, 0.638954005f, -0.051000000f},
    {24.0f, 0.619406152f, 0.654260706f, -0.053270000f},
    {24.5f, 0.632448749f, 0.669686818f, -0.055610000f},
    {25.0f, 0.645497871f, 0.685236122f, -0.057990000f},
    {25.5f, 0.658553478f, 0.700912495f, -0.060430000f},
    {26.0f, 0.671615522f, 0.716719920f, -0.062930000f},
    {26.5f, 0.684683951f, 0.732662485f, -0.065490000f},
    {27.0f, 0.697758705f, 0.748744390f, -0.068090000f},
    {27.5f, 0.710839717f, 0.764969952f, -0.070760000f},
    {28.0f, 0.723926914f, 0.781343610f, -0.073480000f},
    {28.5f, 0.737020217f, 0.797869929f, -0.076270000f},
    {29.0f, 0.750119539f, 0.814553607f, -0.079100000f},
    {29.5f, 0.763224787f, 0.831399480f, -0.082000000f},
    {30.0f, 0.776335861f, 0.848412529f, -0.084950000f},
    {30.5f, 0.789452657f, 0.865597885f, -0.087970000f},
    {31.0f, 0.802575061f, 0.882960837f, -0.091040000f},
    {31.5f, 0.815702955f, 0.900506840f, -0.094170000f},
    {32.0f, 0.828836212f, 0.918241517f, -0.097370000f},
    {32.5f, 0.841974699f, 0.936170674f, -0.100620000f},
    {33.0f, 0.855118277f, 0.954300306f, -0.103930000f},
    {33.5f, 0.868266800f, 0.972636600f, -0.107310000f},
    {34.0f, 0.881420114f, 0.991185952f, -0.110740000f},
    {34.5f, 0.894578058f, 1.009954974f, -0.114240000f},
    {35.0f, 0.907740463f, 1.028950500f, -0.117800000f},
    {35.5f, 0.920907155f, 1.048179602f, -0.121420000f},
    {36.0f, 0.934077949f, 1.067649599f, -0.125110000f},
    {36.5f, 0.947252653f, 1.087368068f, -0.128860000f},
    {37.0f, 0.960431069f, 1.107342858f, -0.132670000f},
    {37.5f, 0.973612987f, 1.127582102f, -0.136550000f},
    {38.0f, 0.986798190f, 1.148094232f, -0.140490000f},
    {38.5f, 0.999986453f, 1.168887993f, -0.144500000f},
    {39.0f, 1.013177540f, 1.189972459f, -0.148570000f},
    {39.5f, 1.026371206f, 1.211357048f, -0.152710000f},
    {40.0f, 1.039567197f, 1.233051542f, -0.156920000f},
    {40.5f, 1.052765246f, 1.255066106f, -0.161190000f},
    {41.0f, 1.065965078f, 1.277411302f, -0.165530000f},
    {41.5f, 1.079166405f, 1.300098119f, -0.169930000f},
    {42.0f, 1.092368929f, 1.323137985f, -0.174410000f},
    {42.5f, 1.105572339f, 1.346542801f, -0.178950000f},
    {43.0f, 1.118776312f, 1.370324956f, -0.183570000f},
    {43.5f, 1.131980511f, 1.394497363f, -0.188250000f},
    {44.0f, 1.145184587f, 1.419073479f, -0.193010000f},
    {44.5f, 1.158388177f, 1.444067344f, -0.197830000f},
    {45.0f, 1.171590903f, 1.469493605f, -0.202720000f},
    {45.5f, 1.184792373f, 1.495367557f, -0.207690000f},
    {46.0f, 1.197992177f, 1.521705174f, -0.212730000f},
    {46.5f, 1.211189893f, 1.548523156f, -0.217840000f},
    {47.0f, 1.224385080f, 1.575838962f, -0.223030000f},
    {47.5f, 1.237577278f, 1.603670864f, -0.228280000f},
    {48.0f, 1.250766015f, 1.632037989f, -0.233620000f},
    {48.5f, 1.263950794f, 1.660960373f, -0.239020000f},
    {49.0f, 1.277131103f, 1.690459018f, -0.244510000f},
    {49.5f, 1.290306411f, 1.720555950f, -0.250060000f},
    {50.0f, 1.303476164f, 1.751274284f, -0.255700000f},
    {50.5f, 1.316642120f, 1.782638290f, -0.261410000f},
    {51.0f, 1.329801126f, 1.814673475f, -0.267200000f},
    {51.5f, 1.342952550f, 1.847406654f, -0.273060000f},
    {52.0f, 1.356095736f, 1.880866044f, -0.279000000f},
    {52.5f, 1.369230005f, 1.915081352f, -0.285030000f},
    {53.0f, 1.382354658f, 1.950083879f, -0.291130000f},
    {53.5f, 1.395468969f, 1.985906631f, -0.297310000f},
    {54.0f, 1.408572190f, 2.022584431f, -0.303580000f},
    {54.5f, 1.421663546f, 2.060154054f, -0.309920000f},
    {55.0f, 1.434742238f, 2.098654363f, -0.316350000f},
    {55.5f, 1.447807442f, 2.138126463f, -0.322860000f},
    {56.0f, 1.460858306f, 2.178613863f, -0.329460000f},
    {56.5f, 1.473893952f, 2.220162656f, -0.336130000f},
    {57.0f, 1.486913475f, 2.262821717f, -0.342890000f},
    {57.5f, 1.499915940f, 2.306642918f, -0.349740000f},
    {58.0f, 1.512900388f, 2.351681357f, -0.356670000f},
    {58.5f, 1.525865827f, 2.397995619f, -0.363690000f},
    {59.0f, 1.538811239f, 2.445648057f, -0.370800000f},
    {59.5f, 1.551735575f, 2.494705098f, -0.377990000f},
    {60.0f, 1.564637757f, 2.545237586f, -0.385270000f},
    {60.5f, 1.577517602f, 2.597321154f, -0.392640000f},
    {61.0f, 1.590375057f, 2.651036640f, -0.400090000f},
    {61.5f, 1.603208958f, 2.706470540f, -0.407640000f},
    {62.0f, 1.616018098f, 2.763715514f, -0.415270000f},
    {62.5f, 1.628801228f, 2.822870952f, -0.423000000f},
    {63.0f, 1.641557054f, 2.884043588f, -0.430810000f},
    {63.5f, 1.654284239f, 2.947348201f, -0.438720000f},
    {64.0f, 1.666981404f, 3.012908384f, -0.446720000f},
    {64.5f, 1.679647124f, 3.080857412f, -0.454810000f},
    {65.0f, 1.692279933f, 3.151339206f, -0.463000000f},
    {65.5f, 1.704878323f, 3.224509423f, -0.471280000f},
    {66.0f, 1.717440742f, 3.300536677f, -0.479650000f},
    {66.5f, 1.729965600f, 3.379603917f, -0.488120000f},
    {67.0f, 1.742451267f, 3.461909987f, -0.496680000f},
    {67.5f, 1.754896071f, 3.547671392f, -0.505340000f},
    {68.0f, 1.767298307f, 3.637124304f, -0.514090000f},
    {68.5f, 1.779656233f, 3.730526849f, -0.522950000f},
    {69.0f, 1.791968073f, 3.828161722f, -0.531900000f},
    {69.5f, 1.804232020f, 3.930339182f, -0.540950000f},
    {70.0f, 1.816446238f, 4.037400499f, -0.550100000f},
    {70.5f, 1.828562175f, 4.149721928f, -0.559350000f},
    {71.0f, 1.840623989f, 4.267719313f, -0.568710000f},
    {71.5f, 1.852629979f, 4.391853441f, -0.578170000f},
    {72.0f, 1.864578424f, 4.522636277f, -0.587720000f},
    {72.5f, 1.876467581f, 4.660638281f, -0.597380000f},
    {73.0f, 1.888295687f, 4.806497007f, -0.607140000f},
    {73.5f, 1.900060962f, 4.960927272f, -0.616990000f},
    {74.0f, 1.911761607f, 5.124733225f, -0.626950000f},
    {74.5f, 1.923395805f, 5.298822758f, -0.637010000f},
    {75.0f, 1.934961727f, 5.484224797f, -0.647180000f},
    {75.5f, 1.946457525f, 5.682110167f, -0.657440000f},
    {76.0f, 1.957881342f, 5.893816934f, -0.667810000f},
    {76.5f, 1.969231308f, 6.120881377f, -0.678280000f},
    {77.0f, 1.980505540f, 6.365076099f, -0.688850000f},
    {77.5f, 1.991702150f, 6.628457302f, -0.699520000f},
    {78.0f, 2.002819238f, 6.913423862f, -0.710300000f},
    {78.5f, 2.013854902f, 7.222791827f, -0.721180000f},
    {79.0f, 2.024807230f, 7.559889229f, -0.732160000f},
    {79.5f, 2.035674312f, 7.928677985f, -0.743250000f},
    {80.0f, 2.046454232f, 8.333912368f, -0.754440000f},
    {80.5f, 2.057170610f, 8.781347521f, -0.765730000f},
    {81.0f, 2.067795270f, 9.278017477f, -0.777130000f},
    {81.5f, 2.078326173f, 9.832611305f, -0.788630000f},
    {82.0f, 2.088761363f, 10.455990310f, -0.800230000f},
    {82.5f, 2.099098977f, 11.161912100f, -0.811940000f},
    {83.0f, 2.109337254f, 11.968065000f, -0.823750000f},
    {83.5f, 2.119474540f, 12.897579850f, -0.835670000f},
    {84.0f, 2.129509299f, 13.981297720f, -0.847690000f},
    {84.5f, 2.139440114f, 15.261274600f, -0.859810000f},
    {85.0f, 2.149265689f, 16.796388770f, -0.872040000f},
    {85.5f, 2.158984854f, 18.671686610f, -0.884370000f},
    {86.0f, 2.168596557f, 21.014737620f, -0.896810000f},
    {86.5f, 2.178099864f, 24.026008070f, -0.909340000f},
    {87.0f, 2.187493950f, 28.039608350f, -0.921990000f},
    {87.5f, 2.196778087f, 33.656937010f, -0.934730000f},
    {88.0f, 2.205951632f, 42.080791080f, -0.947580000f},
    {88.5f, 2.215014009f, 56.117696740f, -0.960530000f},
    {89.0f, 2.223964688f, 84.187232270f, -0.973580000f},
    {89.5f, 2.232803161f, 168.387288600f, -0.986740000f},
    {90.0f, 2.238082725f, 26350.990230000f, -0.999900000f},
};

/* Fallback camera calibrations. Runtime calib_videoN.yaml files can override
 * these values through fisheye_load_calibration_dir(). */
fisheye_cam_t g_fisheye_cams[4] = {
    { .cx = 970.8f, .cy = 542.8f, .focal = 558.4f,
      .k = { 0.057900f, -0.093996f, -0.015318f, 0.129271f },
      .src_w = 1920, .src_h = 1080 },
    { .cx = 986.1f, .cy = 513.7f, .focal = 538.4f,
      .k = { 0.037885f, -0.012286f, 0.000853f, -0.004309f },
      .src_w = 1920, .src_h = 1080 },
    { .cx = 959.0f, .cy = 541.2f, .focal = 532.4f,
      .k = { 0.051246f, -0.036630f, 0.020151f, -0.008140f },
      .src_w = 1920, .src_h = 1080 },
    { .cx = 960.0f, .cy = 540.0f, .focal = 533.0f,
      .k = { 0.012548f, -0.067906f, 0.114628f, -0.050734f },
      .src_w = 1920, .src_h = 1080 },
};

static int scan_floats_from_line(const char *line, float *out, int max_count)
{
    int n = 0;
    const char *p = line;
    while (*p && n < max_count) {
        while (*p && !((*p >= '0' && *p <= '9') ||
                       ((*p == '-' || *p == '+') &&
                        ((p[1] >= '0' && p[1] <= '9') || p[1] == '.')) ||
                       (*p == '.' && (p[1] >= '0' && p[1] <= '9')))) {
            p++;
        }
        if (!*p) break;
        char *endp = NULL;
        double v = strtod(p, &endp);
        if (endp == p) {
            p++;
            continue;
        }
        out[n++] = (float)v;
        p = endp;
    }
    return n;
}

static int load_one_calibration_yaml(const char *path, fisheye_cam_t *cam)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char line[256];
    float k_mat[9] = {0};
    float dist[4] = {0};
    int k_count = 0;
    int d_count = 0;
    int state = 0; /* 0 normal, 1 camera_matrix, 2 dist_coeffs */
    int width = cam->src_w;
    int height = cam->src_h;

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "image_width:")) {
            int v = 0;
            if (sscanf(line, "image_width: %d", &v) == 1 && v > 0) width = v;
            state = 0;
            continue;
        }
        if (strstr(line, "image_height:")) {
            int v = 0;
            if (sscanf(line, "image_height: %d", &v) == 1 && v > 0) height = v;
            state = 0;
            continue;
        }
        if (strstr(line, "camera_matrix:")) {
            state = 1;
            continue;
        }
        if (strstr(line, "dist_coeffs:")) {
            state = 2;
            continue;
        }
        if (state == 1 && k_count < 9) {
            float vals[4];
            int n = scan_floats_from_line(line, vals, 4);
            for (int i = 0; i < n && k_count < 9; i++)
                k_mat[k_count++] = vals[i];
            if (k_count >= 9) state = 0;
            continue;
        }
        if (state == 2 && d_count < 4) {
            float vals[4];
            int n = scan_floats_from_line(line, vals, 4);
            for (int i = 0; i < n && d_count < 4; i++)
                dist[d_count++] = vals[i];
            if (d_count >= 4) state = 0;
            continue;
        }
    }

    fclose(fp);

    if (k_count < 9 || d_count < 4 || width <= 0 || height <= 0) {
        fprintf(stderr, "[fisheye] invalid calibration yaml: %s (K=%d D=%d %dx%d)\n",
                path, k_count, d_count, width, height);
        return -1;
    }

    float fx = k_mat[0];
    float fy = k_mat[4];
    cam->focal = (fx + fy) * 0.5f;
    cam->cx = k_mat[2];
    cam->cy = k_mat[5];
    for (int i = 0; i < 4; i++)
        cam->k[i] = dist[i];
    cam->src_w = width;
    cam->src_h = height;

    printf("[fisheye] loaded %s: size=%dx%d fx=%.2f fy=%.2f focal=%.2f cx=%.2f cy=%.2f k=[%.6f %.6f %.6f %.6f]\n",
           path, width, height, fx, fy, cam->focal, cam->cx, cam->cy,
           cam->k[0], cam->k[1], cam->k[2], cam->k[3]);
    return 0;
}

int fisheye_load_calibration_dir(const char *dir, int n_cams)
{
    if (!dir || !dir[0]) return 0;
    if (n_cams > 4) n_cams = 4;

    int loaded = 0;
    for (int i = 0; i < n_cams; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/calib_video%d.yaml", dir, i);
        if (load_one_calibration_yaml(path, &g_fisheye_cams[i]) == 0)
            loaded++;
    }
    printf("[fisheye] calibration dir=%s loaded=%d/%d\n", dir, loaded, n_cams);
    return loaded;
}

/* ------------------------------------------------------------------ */
/* Lens lookup (Kannala-Brandt)                                       */
/* ------------------------------------------------------------------ */

float lens_6028_radius(float angle_rad, const fisheye_cam_t *cam)
{
    float t2  = angle_rad * angle_rad;
    float t4  = t2 * t2;
    float t6  = t4 * t2;
    float t8  = t4 * t4;
    float td  = angle_rad * (1.0f + cam->k[0]*t2 + cam->k[1]*t4
                                     + cam->k[2]*t6 + cam->k[3]*t8);
    return cam->focal * td;
}

/* ------------------------------------------------------------------ */
/* Apply rotation & flip to source UV coordinate                      */
/* ------------------------------------------------------------------ */
static void apply_rotate_flip(float *u, float *v,
                               int rotate_deg, bool flip_x, bool flip_y)
{
    float tu = *u, tv = *v;

    /* Center around 0.5 */
    tu -= 0.5f; tv -= 0.5f;

    /* Rotate */
    float ru, rv;
    switch (rotate_deg) {
    case 90:  ru = -tv; rv =  tu; break;
    case 180: ru = -tu; rv = -tv; break;
    case 270: ru =  tv; rv = -tu; break;
    default:  ru =  tu; rv =  tv; break;
    }

    /* Flip */
    if (flip_x) ru = -ru;
    if (flip_y) rv = -rv;

    *u = ru + 0.5f;
    *v = rv + 0.5f;
}

/* ------------------------------------------------------------------ */
/* Extended mesh generator with rotate/flip + UV stats                */
/* ------------------------------------------------------------------ */

#define MESH_DIV 48

int fisheye_mesh_build_ex(fisheye_mesh_t *mesh,
                          const fisheye_cam_t *cam,
                          float tile_x0, float tile_y0,
                          float tile_w,  float tile_h,
                          int out_w, int out_h,
                          float fov_h,
                          int rotate_deg, bool flip_x, bool flip_y,
                          fisheye_uv_stats_t *stats)
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
    int oob_top = 0, oob_bot = 0, oob_left = 0, oob_right = 0;
    int near_edge = 0;
    float u_min = 999, u_max = -999, v_min = 999, v_max = -999;

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

            /* Kannala-Brandt distorted radius */
            float r_real = lens_6028_radius(inc_angle, cam);

            /* Source pixel in the fisheye image */
            float sx, sy;
            if (r_ideal > 1e-6f) {
                sx = cam->cx + dx * (r_real / r_ideal);
                sy = cam->cy + dy * (r_real / r_ideal);
            } else {
                sx = cam->cx;
                sy = cam->cy;
            }

            /* Convert to normalized UV */
            float uv_u = sx / (float)cam->src_w;
            float uv_v = sy / (float)cam->src_h;

            /* Apply rotation and flip */
            apply_rotate_flip(&uv_u, &uv_v, rotate_deg, flip_x, flip_y);

            tex[vi * 2 + 0] = uv_u;
            tex[vi * 2 + 1] = uv_v;

            /* Statistics */
            if (uv_u < u_min) u_min = uv_u;
            if (uv_u > u_max) u_max = uv_u;
            if (uv_v < v_min) v_min = uv_v;
            if (uv_v > v_max) v_max = uv_v;

            bool in_bounds = (uv_u >= 0.0f && uv_u <= 1.0f &&
                              uv_v >= 0.0f && uv_v <= 1.0f);
            if (in_bounds) {
                valid++;
                float u_edge = (uv_u < 0.05f || uv_u > 0.95f);
                float v_edge = (uv_v < 0.05f || uv_v > 0.95f);
                if (u_edge || v_edge) near_edge++;
            } else {
                if (uv_v > 1.0f) oob_top++;
                if (uv_v < 0.0f) oob_bot++;
                if (uv_u < 0.0f) oob_left++;
                if (uv_u > 1.0f) oob_right++;
            }
        }
    }

    /* Generate triangle indices */
    int ii = 0;
    for (int j = 0; j < gh; j++) {
        for (int i = 0; i < gw; i++) {
            int a = j * (gw + 1) + i;
            int b = a + 1, c = a + (gw + 1), d = c + 1;
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

    if (stats) {
        memset(stats, 0, sizeof(*stats));
        stats->total_verts    = nv;
        stats->valid_pct      = (float)valid * 100.0f / (float)nv;
        stats->oob_pct        = (float)(nv - valid) * 100.0f / (float)nv;
        stats->edge_top_pct   = (float)oob_top * 100.0f / (float)nv;
        stats->edge_bot_pct   = (float)oob_bot * 100.0f / (float)nv;
        stats->edge_left_pct  = (float)oob_left * 100.0f / (float)nv;
        stats->edge_right_pct = (float)oob_right * 100.0f / (float)nv;
        stats->near_edge_pct  = (float)near_edge * 100.0f / (float)nv;
        stats->u_min = u_min; stats->u_max = u_max;
        stats->v_min = v_min; stats->v_max = v_max;
    }

    printf("[mesh] cam: cx=%.1f cy=%.1f focal=%.1f fov=%.0f rot=%d flip=%d,%d  "
           "verts=%d valid=%.1f%% oob=%.1f%% "
           "UV:[%.2f-%.2f,%.2f-%.2f] "
           "edges: T=%.1f%% B=%.1f%% L=%.1f%% R=%.1f%% near=%.1f%%\n",
           cam->cx, cam->cy, cam->focal, fov_h, rotate_deg, flip_x, flip_y,
           nv, stats ? stats->valid_pct : (float)valid * 100.0f / nv,
           stats ? stats->oob_pct : (float)(nv - valid) * 100.0f / nv,
           u_min, u_max, v_min, v_max,
           stats ? stats->edge_top_pct : 0.0f,
           stats ? stats->edge_bot_pct : 0.0f,
           stats ? stats->edge_left_pct : 0.0f,
           stats ? stats->edge_right_pct : 0.0f,
           stats ? stats->near_edge_pct : 0.0f);

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

/* ------------------------------------------------------------------ */
/* UV debug PPM — color-coded mesh coverage visualization             */
/* ------------------------------------------------------------------ */
void fisheye_mesh_dump_uv_debug(const char *path,
                                const fisheye_uv_stats_t *stats,
                                const fisheye_cam_t *cam,
                                float fov_h, int out_w, int out_h,
                                int rotate_deg, bool flip_x, bool flip_y)
{
    (void)stats;  /* stats is for logging; we recompute per-pixel here */

    FILE *fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "[mesh] cannot open %s\n", path); return; }

    fprintf(fp, "P6\n%d %d\n255\n", out_w, out_h);

    float fov_v = fov_h * (float)out_h / (float)out_w;
    float fov_h_rad = fov_h * (float)(M_PI / 180.0);
    float fov_v_rad = fov_v * (float)(M_PI / 180.0);

    unsigned char *row = (unsigned char *)malloc(out_w * 3);
    if (!row) { fclose(fp); return; }

    for (int j = 0; j < out_h; j++) {
        for (int i = 0; i < out_w; i++) {
            float u = ((float)i + 0.5f) / (float)out_w;
            float v = ((float)j + 0.5f) / (float)out_h;

            float theta = (u - 0.5f) * fov_h_rad;
            float phi   = (v - 0.5f) * fov_v_rad;
            float cos_theta = cosf(theta);

            float dx = cam->focal * tanf(theta);
            float dy = cam->focal * tanf(phi) / cos_theta;
            float r_ideal = sqrtf(dx * dx + dy * dy);
            float inc_angle = atan2f(r_ideal, cam->focal);
            float r_real = lens_6028_radius(inc_angle, cam);

            float sx, sy;
            if (r_ideal > 1e-6f) {
                sx = cam->cx + dx * (r_real / r_ideal);
                sy = cam->cy + dy * (r_real / r_ideal);
            } else {
                sx = cam->cx; sy = cam->cy;
            }

            float uv_u = sx / (float)cam->src_w;
            float uv_v = sy / (float)cam->src_h;
            apply_rotate_flip(&uv_u, &uv_v, rotate_deg, flip_x, flip_y);

            unsigned char r, g, b;
            bool in = (uv_u >= 0.0f && uv_u <= 1.0f && uv_v >= 0.0f && uv_v <= 1.0f);
            bool near = (uv_u >= -0.05f && uv_u <= 1.05f && uv_v >= -0.05f && uv_v <= 1.05f);

            if (in) {
                /* Green for valid, darker near edges */
                float edge_dist = fminf(fminf(uv_u, 1.0f - uv_u), fminf(uv_v, 1.0f - uv_v));
                if (edge_dist < 0.05f) { r = 0; g = 128; b = 255; }  /* blue = near edge */
                else                    { r = 0; g = 200; b = 0; }     /* green = good */
            } else if (near) {
                r = 255; g = 128; b = 0;  /* orange = just OOB */
            } else {
                r = 255; g = 30; b = 30;  /* red = far OOB */
            }
            row[i*3] = r; row[i*3+1] = g; row[i*3+2] = b;
        }
        fwrite(row, 1, out_w * 3, fp);
    }

    free(row);
    fclose(fp);
    printf("[mesh] UV debug PPM saved: %s (%dx%d)\n", path, out_w, out_h);
}
