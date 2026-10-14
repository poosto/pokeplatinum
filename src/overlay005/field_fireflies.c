#include "overlay005/field_fireflies.h"

#include <nitro.h>
#include <nnsys.h>

#include "constants/field/map.h"

#include "field/field_system.h"
#include "overlay005/field_point_lights.h"

#include "map_tile_behavior.h"
#include "player_avatar.h"
#include "terrain_collision_manager.h"

#ifdef SDK_PORT
#include <simulator/sim_g3_lights.h>
#endif

#define MAX_FIREFLIES         16
#define SPAWN_RANGE_TILES     7
#define LIFETIME_MIN_FRAMES   240
#define LIFETIME_RANGE_FRAMES 240
#define FADE_FRAMES           40
#define LIGHT_RADIUS          FX32_CONST(28)
#define FIREFLY_COLOR_R       22
#define FIREFLY_COLOR_G       31
#define FIREFLY_COLOR_B       8
// The light is dimmer than the dot itself, it only tints the grass around it
#define LIGHT_STRENGTH FX32_CONST(0.6)

typedef struct Firefly {
    BOOL active;
    VecFx32 base;
    u16 age;
    u16 lifetime;
    u16 wanderPhase[3];
    u16 wanderSpeed[3];
    u16 blinkPhase;
    u16 blinkSpeed;
} Firefly;

static Firefly sFireflies[MAX_FIREFLIES];

// Separate from the game's RNG so fireflies never change encounters or anything
// else that consumes random numbers
static u32 sRandState = 0x2F6B1C3D;

static u32 Rand(void)
{
    sRandState = sRandState * 1103515245 + 12345;
    return sRandState >> 16;
}

static int RandRange(int min, int max)
{
    return min + (int)(Rand() % (u32)(max - min + 1));
}

static void Fireflies_TrySpawn(FieldSystem *fieldSystem, const VecFx32 *playerPos)
{
    Firefly *firefly = NULL;

    for (int i = 0; i < MAX_FIREFLIES; i++) {
        if (!sFireflies[i].active) {
            firefly = &sFireflies[i];
            break;
        }
    }

    if (firefly == NULL) {
        return;
    }

    int tileX = playerPos->x / MAP_OBJECT_TILE_SIZE + RandRange(-SPAWN_RANGE_TILES, SPAWN_RANGE_TILES);
    int tileZ = playerPos->z / MAP_OBJECT_TILE_SIZE + RandRange(-SPAWN_RANGE_TILES, SPAWN_RANGE_TILES);

    if (tileX < 0 || tileZ < 0) {
        return;
    }

    u8 behavior = TerrainCollisionManager_GetTileBehavior(fieldSystem, tileX, tileZ);

    if (!TileBehavior_IsTallGrass(behavior) && !TileBehavior_IsVeryTallGrass(behavior)) {
        return;
    }

    u8 heightSource;
    firefly->base.x = tileX * MAP_OBJECT_TILE_SIZE + RandRange(2, 14) * FX32_ONE;
    firefly->base.z = tileZ * MAP_OBJECT_TILE_SIZE + RandRange(2, 14) * FX32_ONE;
    firefly->base.y = TerrainCollisionManager_GetHeight(fieldSystem, playerPos->y, firefly->base.x, firefly->base.z, &heightSource)
        + RandRange(8, 22) * FX32_ONE;

    firefly->active = TRUE;
    firefly->age = 0;
    firefly->lifetime = LIFETIME_MIN_FRAMES + RandRange(0, LIFETIME_RANGE_FRAMES);

    for (int i = 0; i < 3; i++) {
        firefly->wanderPhase[i] = Rand();
        firefly->wanderSpeed[i] = RandRange(150, 450);
    }

    firefly->blinkPhase = Rand();
    firefly->blinkSpeed = RandRange(400, 900);
}

// 0 to FX32_ONE, fading in at the start of its life and out at the end
static fx32 Firefly_GetFade(const Firefly *firefly)
{
    int frames = firefly->age < FADE_FRAMES ? firefly->age : firefly->lifetime - firefly->age;

    if (frames >= FADE_FRAMES) {
        return FX32_ONE;
    }

    return (frames << FX32_SHIFT) / FADE_FRAMES;
}

static int ScaleChannel(int channel, fx32 scale)
{
    return (channel * scale + (FX32_ONE >> 1)) >> FX32_SHIFT;
}

void FieldFireflies_Update(FieldSystem *fieldSystem)
{
#ifdef SDK_PORT
    SIM_G3GlowSprite sprites[MAX_FIREFLIES];
    int spriteCount = 0;
    fx32 night = FieldPointLights_GetNightBrightness();

    if (night == 0) {
        for (int i = 0; i < MAX_FIREFLIES; i++) {
            sFireflies[i].active = FALSE;
        }

        SIM_G3_SetGlowSprites(sprites, 0);
        return;
    }

    const VecFx32 *playerPos = PlayerAvatar_GetPos(fieldSystem->playerAvatar);

    // A few spawn attempts per frame, most land on tiles that aren't grass
    for (int i = 0; i < 4; i++) {
        Fireflies_TrySpawn(fieldSystem, playerPos);
    }

    for (int i = 0; i < MAX_FIREFLIES; i++) {
        Firefly *firefly = &sFireflies[i];

        if (!firefly->active) {
            continue;
        }

        if (++firefly->age >= firefly->lifetime) {
            firefly->active = FALSE;
            continue;
        }

        for (int j = 0; j < 3; j++) {
            firefly->wanderPhase[j] += firefly->wanderSpeed[j];
        }

        firefly->blinkPhase += firefly->blinkSpeed;

        // Wander around the spawn point, more sideways than up and down
        VecFx32 pos = firefly->base;
        pos.x += FX_SinIdx(firefly->wanderPhase[0]) * 10;
        pos.y += FX_SinIdx(firefly->wanderPhase[1]) * 4;
        pos.z += FX_SinIdx(firefly->wanderPhase[2]) * 10;

        // Squared so the glow pulses briefly rather than staying half lit
        fx32 pulse = (FX_SinIdx(firefly->blinkPhase) + FX32_ONE) / 2;
        pulse = FX_Mul(pulse, pulse);

        fx32 brightness = FX_Mul(FX_Mul(Firefly_GetFade(firefly), night), FX32_CONST(0.25) + FX_Mul(pulse, FX32_CONST(0.75)));
        GXRgb color = GX_RGB(ScaleChannel(FIREFLY_COLOR_R, brightness), ScaleChannel(FIREFLY_COLOR_G, brightness), ScaleChannel(FIREFLY_COLOR_B, brightness));

        SIM_G3GlowSprite *sprite = &sprites[spriteCount++];
        MTX_MultVec43(&pos, NNS_G3dGlbGetCameraMtx(), &sprite->viewPos);
        sprite->color = color;

        fx32 lightBrightness = FX_Mul(brightness, LIGHT_STRENGTH);
        FieldPointLights_Add(&pos, LIGHT_RADIUS, GX_RGB(ScaleChannel(FIREFLY_COLOR_R, lightBrightness), ScaleChannel(FIREFLY_COLOR_G, lightBrightness), ScaleChannel(FIREFLY_COLOR_B, lightBrightness)));
    }

    SIM_G3_SetGlowSprites(sprites, spriteCount);
#endif
}
