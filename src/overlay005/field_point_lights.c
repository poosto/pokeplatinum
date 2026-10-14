#include "overlay005/field_point_lights.h"

#include <nitro.h>
#include <nnsys.h>
#include <string.h>

#include "overlay005/land_data.h"

#include "rtc.h"

#ifdef SDK_PORT
#include <simulator/sim_g3_lights.h>
#endif

#define SECONDS_PER_HOUR (60 * 60)
#define LAMP_RADIUS      FX32_CONST(96)
#define LAMP_COLOR_R     31
#define LAMP_COLOR_G     25
#define LAMP_COLOR_B     14

#define WINDOW_COLOR        GX_RGB(31, 24, 12)
#define WINDOW_MAX_STRENGTH FX32_CONST(0.85)

// When lamps fade in and out, in seconds since midnight
#define LAMPS_ON_START  (17 * SECONDS_PER_HOUR + 30 * 60)
#define LAMPS_ON_END    (19 * SECONDS_PER_HOUR + 30 * 60)
#define LAMPS_OFF_START (4 * SECONDS_PER_HOUR)
#define LAMPS_OFF_END   (6 * SECONDS_PER_HOUR)

typedef struct FieldMapLamp {
    u16 landDataID;
    VecFx32 position; // Relative to the map's rendering position
} FieldMapLamp;

// Street lamps are part of the terrain models rather than map props
#include "data/field_map_lamps.h"

#ifdef SDK_PORT
static SIM_G3PointLight sLights[SIM_G3_MAX_POINT_LIGHTS];
#endif
static int sLightCount;
static GXRgb sLampColor;
static BOOL sWindowsGlowing;
static fx32 sNightBrightness;

// How strongly lamps are lit at the current time, from 0 to FX32_ONE
static fx32 FieldPointLights_CalcBrightness(void)
{
    int seconds = GetSecondsSinceMidnight();

    if (seconds >= LAMPS_ON_END || seconds < LAMPS_OFF_START) {
        return FX32_ONE;
    }

    if (seconds >= LAMPS_ON_START) {
        return ((seconds - LAMPS_ON_START) << FX32_SHIFT) / (LAMPS_ON_END - LAMPS_ON_START);
    }

    if (seconds < LAMPS_OFF_END) {
        return ((LAMPS_OFF_END - seconds) << FX32_SHIFT) / (LAMPS_OFF_END - LAMPS_OFF_START);
    }

    return 0;
}

static int ScaleChannel(int channel, fx32 brightness)
{
    return (channel * brightness + (FX32_ONE >> 1)) >> FX32_SHIFT;
}

void FieldPointLights_AddMapLamps(int landDataID, const VecFx32 *mapPosition)
{
#ifdef SDK_PORT
    for (int i = 0; i < NELEMS(sFieldMapLamps) && sLightCount < SIM_G3_MAX_POINT_LIGHTS; i++) {
        if (sFieldMapLamps[i].landDataID != landDataID) {
            continue;
        }

        VecFx32 worldPos;
        VEC_Add(mapPosition, &sFieldMapLamps[i].position, &worldPos);
        FieldPointLights_Add(&worldPos, LAMP_RADIUS, sLampColor);
    }
#endif
}

void FieldPointLights_Add(const VecFx32 *worldPos, fx32 radius, GXRgb color)
{
#ifdef SDK_PORT
    if (sLightCount >= SIM_G3_MAX_POINT_LIGHTS) {
        return;
    }

    SIM_G3PointLight *light = &sLights[sLightCount++];
    MTX_MultVec43(worldPos, NNS_G3dGlbGetCameraMtx(), &light->viewPos);
    light->radius = radius;
    light->color = color;
#endif
}

void FieldPointLights_Submit(void)
{
#ifdef SDK_PORT
    SIM_G3_SetPointLights(sLights, sLightCount);
#endif
}

fx32 FieldPointLights_GetNightBrightness(void)
{
    return sNightBrightness;
}

// Building window materials are named like h_mado (mado is window) or window01
static BOOL IsWindowMaterial(const NNSG3dResName *name)
{
    char str[NNS_G3D_RESNAME_SIZE + 1];
    memcpy(str, name->name, NNS_G3D_RESNAME_SIZE);
    str[NNS_G3D_RESNAME_SIZE] = '\0';

    return strstr(str, "mado") != NULL || strncmp(str, "win", 3) == 0;
}

void FieldPointLights_ApplyWindowGlow(NNSG3dResMdl *model)
{
#ifdef SDK_PORT
    const NNSG3dResMat *materials = NNS_G3dGetMat(model);

    for (u32 i = 0; i < materials->dict.numEntry; i++) {
        if (IsWindowMaterial(NNS_G3dGetMatNameByIdx(materials, i))) {
            NNS_G3dMdlSetMdlPolygonID(model, i, sWindowsGlowing ? SIM_G3_GLOW_POLYGON_ID : 0);
        }
    }
#endif
}

void FieldPointLights_Update(const LandDataManager *landDataMan, BOOL isOutdoors)
{
#ifdef SDK_PORT
    fx32 brightness = isOutdoors ? FieldPointLights_CalcBrightness() : 0;

    sNightBrightness = brightness;
    sWindowsGlowing = brightness != 0;
    SIM_G3_SetGlow(WINDOW_COLOR, FX_Mul(brightness, WINDOW_MAX_STRENGTH));

    sLightCount = 0;
    sLampColor = GX_RGB(ScaleChannel(LAMP_COLOR_R, brightness), ScaleChannel(LAMP_COLOR_G, brightness), ScaleChannel(LAMP_COLOR_B, brightness));

    if (brightness != 0) {
        LandDataManager_AddMapLampPointLights(landDataMan);
    }
#endif
}
