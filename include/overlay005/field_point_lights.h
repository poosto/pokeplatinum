#ifndef POKEPLATINUM_OV5_FIELD_POINT_LIGHTS_H
#define POKEPLATINUM_OV5_FIELD_POINT_LIGHTS_H

#include <nitro/fx/fx.h>

#include "overlay005/land_data.h"

// Lights up lamp posts at night on PC. Called once per frame after the camera
// is computed and before the maps are drawn.
void FieldPointLights_Update(const LandDataManager *landDataMan, BOOL isOutdoors);
void FieldPointLights_AddMapLamps(int landDataID, const VecFx32 *mapPosition);

#endif // POKEPLATINUM_OV5_FIELD_POINT_LIGHTS_H
