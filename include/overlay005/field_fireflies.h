#ifndef POKEPLATINUM_OV5_FIELD_FIREFLIES_H
#define POKEPLATINUM_OV5_FIELD_FIREFLIES_H

#include "field/field_system.h"

// Fireflies drifting over tall grass at night on PC. Called once per frame
// after FieldPointLights_Update, since each firefly also adds a small light.
void FieldFireflies_Update(FieldSystem *fieldSystem);

#endif // POKEPLATINUM_OV5_FIELD_FIREFLIES_H
