#ifndef POKEPLATINUM_OV5_FIELD_POINT_LIGHTS_H
#define POKEPLATINUM_OV5_FIELD_POINT_LIGHTS_H

#include <nitro/fx/fx.h>
#include <nnsys.h>

#include "overlay005/land_data.h"

// Lights up lamp posts at night on PC. Called once per frame after the camera
// is computed and before the maps are drawn, followed by any other lights
// (FieldPointLights_Add) and then FieldPointLights_Submit.
void FieldPointLights_Update(const LandDataManager *landDataMan, BOOL isOutdoors);
void FieldPointLights_Add(const VecFx32 *worldPos, fx32 radius, GXRgb color);
void FieldPointLights_Submit(void);
// How dark it is from the lamps' point of view: 0 by day, FX32_ONE at night
fx32 FieldPointLights_GetNightBrightness(void);
void FieldPointLights_AddMapLamps(int landDataID, const VecFx32 *mapPosition);
// Makes the model's window materials glow while the lamps are on
void FieldPointLights_ApplyWindowGlow(NNSG3dResMdl *model);

#endif // POKEPLATINUM_OV5_FIELD_POINT_LIGHTS_H
