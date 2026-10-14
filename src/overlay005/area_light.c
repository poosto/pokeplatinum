#include "overlay005/area_light.h"

#include <nitro.h>
#include <string.h>

#include "constants/graphics.h"
#include "constants/heap.h"
#include "constants/narc.h"

#include "overlay005/model_attributes.h"

#include "ascii_util.h"
#include "graphics.h"
#include "heap.h"
#include "rtc.h"

#define AREA_LIGHT_FILE_COUNT 4
#define SCRATCH_BUFFER_SIZE   256
#define INVALID_LIGHT_COLOR   0xFFFF

// Template end times are stored in 2-second units
#define TEMPLATE_TIME_TO_SECONDS(time) ((time) * 2)

static int AreaLightManager_FindTemplateIndex(const AreaLightManager *areaLightMan, int currentTime);
static void AreaLightManager_ApplyBlendedTemplateToAreaModelAttrs(AreaLightManager *areaLightMan, int secondsSinceMidnight);
static u32 AreaLightTemplate_New(u32 archiveID, AreaLightTemplate **templates);
static void AreaLightTemplate_Free(AreaLightTemplate **template);
static char *AreaLightTemplate_ParseLightAttrs(char *fileIter, GXRgb *lightColor, VecFx16 *lightVector);
static char *AreaLightTemplate_ParseColor(char *fileIter, GXRgb *color);

AreaLightManager *AreaLightManager_New(ModelAttributes *areaModelAttrs, const u8 archiveID)
{
    GF_ASSERT(archiveID < AREA_LIGHT_FILE_COUNT);

    AreaLightManager *areaLightMan = Heap_Alloc(HEAP_ID_FIELD1, sizeof(AreaLightManager));

    areaLightMan->areaModelAttrs = areaModelAttrs;
    areaLightMan->templateCount = AreaLightTemplate_New(archiveID, &areaLightMan->templates);

    int secondsSinceMidnight = GetSecondsSinceMidnight();
    areaLightMan->activeTemplateIndex = AreaLightManager_FindTemplateIndex(areaLightMan, secondsSinceMidnight / 2);

    areaLightMan->applyToAreaModelAttrs = TRUE;
    AreaLightManager_ApplyBlendedTemplateToAreaModelAttrs(areaLightMan, secondsSinceMidnight);

    return areaLightMan;
}

void AreaLightManager_Free(AreaLightManager **areaLightMan)
{
    GF_ASSERT(areaLightMan);

    AreaLightTemplate_Free(&(*areaLightMan)->templates);
    Heap_FreeExplicit(HEAP_ID_FIELD1, *areaLightMan);

    *areaLightMan = NULL;
}

void AreaLightManager_UpdateActiveTemplate(AreaLightManager *areaLightMan)
{
    GF_ASSERT(areaLightMan);

    int secondsSinceMidnight = GetSecondsSinceMidnight();

    if (areaLightMan->templateCount > 1) {
        // Looked up from scratch rather than stepped, so jumps in time (e.g. the
        // clock override) land on the right template immediately
        areaLightMan->activeTemplateIndex = AreaLightManager_FindTemplateIndex(areaLightMan, secondsSinceMidnight / 2);

        if (areaLightMan->applyToAreaModelAttrs && secondsSinceMidnight != areaLightMan->lastAppliedSeconds) {
            AreaLightManager_ApplyBlendedTemplateToAreaModelAttrs(areaLightMan, secondsSinceMidnight);
        }
    }
}

void AreaLightTemplate_ApplyToModelAttributes(const AreaLightTemplate *template, ModelAttributes *modelAttrs)
{
    for (int i = 0; i < GX_LIGHTS_COUNT; i++) {
        int currentLightMask = (1 << i);

        if ((template->validLightsMask & currentLightMask) != 0) {
            ModelAttributes_SetLightVector(modelAttrs, i, template->lightVectors[i].x, template->lightVectors[i].y, template->lightVectors[i].z);
            ModelAttributes_SetLightColor(modelAttrs, i, template->lightColors[i]);
        } else {
            ModelAttributes_SetLightVector(modelAttrs, i, 0, 0, 0);
            ModelAttributes_SetLightColor(modelAttrs, i, GX_RGB(0, 0, 0));
        }
    }

    ModelAttributes_SetDiffuseReflection(modelAttrs, template->diffuseReflectColor, FALSE, FALSE);
    ModelAttributes_SetAmbientReflection(modelAttrs, template->ambientReflectColor, TRUE);
    ModelAttributes_SetSpecularReflection(modelAttrs, template->specularReflectColor, FALSE, FALSE);
    ModelAttributes_SetEmissionColor(modelAttrs, template->emissionColor, TRUE);
}

static int AreaLightManager_GetTemplateStartTime(const AreaLightManager *areaLightMan, int index)
{
    return index > 0 ? areaLightMan->templates[index - 1].endTime : 0;
}

static int AreaLightManager_FindTemplateIndex(const AreaLightManager *areaLightMan, int currentTime)
{
    for (int i = 0; i < areaLightMan->templateCount; i++) {
        if (areaLightMan->templates[i].endTime > currentTime) {
            return i;
        }
    }

    return areaLightMan->templateCount - 1;
}

// The template that follows the given one, wrapping past midnight. Templates
// whose time range is empty are never active, so they are skipped.
static int AreaLightManager_GetNextTemplateIndex(const AreaLightManager *areaLightMan, int index)
{
    int next = index;

    for (int i = 0; i < areaLightMan->templateCount; i++) {
        next = (next + 1) % areaLightMan->templateCount;

        if (areaLightMan->templates[next].endTime > AreaLightManager_GetTemplateStartTime(areaLightMan, next)) {
            return next;
        }
    }

    return index;
}

static int BlendChannel(int from, int to, fx32 t)
{
    return (from * (FX32_ONE - t) + to * t + (FX32_ONE >> 1)) >> FX32_SHIFT;
}

static GXRgb BlendColor(GXRgb from, GXRgb to, fx32 t)
{
    int r = BlendChannel(from & 0x1F, to & 0x1F, t);
    int g = BlendChannel((from >> 5) & 0x1F, (to >> 5) & 0x1F, t);
    int b = BlendChannel((from >> 10) & 0x1F, (to >> 10) & 0x1F, t);

    return GX_RGB(r, g, b);
}

static void BlendLightVector(const VecFx16 *from, const VecFx16 *to, fx32 t, VecFx16 *out)
{
    out->x = BlendChannel(from->x, to->x, t);
    out->y = BlendChannel(from->y, to->y, t);
    out->z = BlendChannel(from->z, to->z, t);

    if (out->x != 0 || out->y != 0 || out->z != 0) {
        VEC_Fx16Normalize(out, out);
    }
}

// Lights the area with the active template blended toward the next one by how
// far into the active template's time range we are, so the lighting changes
// smoothly instead of snapping when a template ends
static void AreaLightManager_ApplyBlendedTemplateToAreaModelAttrs(AreaLightManager *areaLightMan, int secondsSinceMidnight)
{
    int activeIndex = areaLightMan->activeTemplateIndex;
    const AreaLightTemplate *from = &areaLightMan->templates[activeIndex];
    const AreaLightTemplate *to = &areaLightMan->templates[AreaLightManager_GetNextTemplateIndex(areaLightMan, activeIndex)];

    int startSeconds = TEMPLATE_TIME_TO_SECONDS(AreaLightManager_GetTemplateStartTime(areaLightMan, activeIndex));
    int endSeconds = TEMPLATE_TIME_TO_SECONDS(from->endTime);
    fx32 t = 0;

    if (endSeconds > startSeconds) {
        t = ((secondsSinceMidnight - startSeconds) << FX32_SHIFT) / (endSeconds - startSeconds);
        t = MATH_CLAMP(t, 0, FX32_ONE);
    }

    AreaLightTemplate blended;
    blended.endTime = from->endTime;
    blended.validLightsMask = from->validLightsMask | to->validLightsMask;

    for (int i = 0; i < GX_LIGHTS_COUNT; i++) {
        int lightMask = 1 << i;
        BOOL fromValid = (from->validLightsMask & lightMask) != 0;
        BOOL toValid = (to->validLightsMask & lightMask) != 0;

        // Invalid lights have a black color, so a light turning on or off fades
        // in or out while keeping the direction of whichever side has one
        blended.lightColors[i] = BlendColor(from->lightColors[i], to->lightColors[i], t);

        if (fromValid && toValid) {
            BlendLightVector(&from->lightVectors[i], &to->lightVectors[i], t, &blended.lightVectors[i]);
        } else {
            blended.lightVectors[i] = fromValid ? from->lightVectors[i] : to->lightVectors[i];
        }
    }

    blended.diffuseReflectColor = BlendColor(from->diffuseReflectColor, to->diffuseReflectColor, t);
    blended.ambientReflectColor = BlendColor(from->ambientReflectColor, to->ambientReflectColor, t);
    blended.specularReflectColor = BlendColor(from->specularReflectColor, to->specularReflectColor, t);
    blended.emissionColor = BlendColor(from->emissionColor, to->emissionColor, t);

    AreaLightTemplate_ApplyToModelAttributes(&blended, areaLightMan->areaModelAttrs);
    areaLightMan->lastAppliedSeconds = secondsSinceMidnight;
}

void AreaLight_UseGlobalModelAttributes(NNSG3dResMdl *model)
{
    NNS_G3dMdlUseGlbDiff(model);
    NNS_G3dMdlUseGlbAmb(model);
    NNS_G3dMdlUseGlbSpec(model);
    NNS_G3dMdlUseGlbEmi(model);
}

static u32 AreaLightTemplate_New(u32 archiveID, AreaLightTemplate **templates)
{
    int i, j;
    char lineBuffer[SCRATCH_BUFFER_SIZE];
    char endTimeBuffer[SCRATCH_BUFFER_SIZE];

    void *fileBuffer = LoadMemberFromNARC(NARC_INDEX_DATA__AREALIGHT, archiveID, FALSE, HEAP_ID_FIELD1, FALSE);
    void *fileIter = fileBuffer;
    int templateCount = 0;

    do {
        fileIter = Ascii_CopyToTerminator(fileIter, lineBuffer, '\r');

        if (!(lineBuffer[0] == 'E' && lineBuffer[1] == 'O' && lineBuffer[2] == 'F')) {
            fileIter = Ascii_CopyToTerminator(fileIter, lineBuffer, '\r');
            fileIter = Ascii_CopyToTerminator(fileIter, lineBuffer, '\r');
            fileIter = Ascii_CopyToTerminator(fileIter, lineBuffer, '\r');
            fileIter = Ascii_CopyToTerminator(fileIter, lineBuffer, '\r');
            fileIter = Ascii_CopyToTerminator(fileIter, lineBuffer, '\r');
            fileIter = Ascii_CopyToTerminator(fileIter, lineBuffer, '\r');
            fileIter = Ascii_CopyToTerminator(fileIter, lineBuffer, '\r');
            fileIter = Ascii_CopyToTerminator(fileIter, lineBuffer, '\r');
            fileIter = Ascii_CopyToTerminator(fileIter, lineBuffer, '\r');
            templateCount++;
        }
    } while (!(lineBuffer[0] == 'E' && lineBuffer[1] == 'O' && lineBuffer[2] == 'F'));

    *templates = Heap_Alloc(HEAP_ID_FIELD1, sizeof(AreaLightTemplate) * templateCount);
    MI_CpuClear8(*templates, sizeof(AreaLightTemplate) * templateCount);
    fileIter = fileBuffer;

    for (i = 0; i < templateCount; i++) {
        AreaLightTemplate *template = &(*templates)[i];
        fileIter = Ascii_CopyToTerminator(fileIter, lineBuffer, '\r');
        char *iter = lineBuffer;
        iter = Ascii_CopyToTerminator(iter, endTimeBuffer, ',');
        template->endTime = Ascii_ConvertToInt(endTimeBuffer);

        for (j = 0; j < GX_LIGHTS_COUNT; j++) {
            fileIter = AreaLightTemplate_ParseLightAttrs(fileIter, &template->lightColors[j], &template->lightVectors[j]);

            if (template->lightColors[j] != INVALID_LIGHT_COLOR) {
                template->validLightsMask |= 1 << j;
            } else {
                template->lightColors[j] = GX_RGB(0, 0, 0);
            }
        }

        fileIter = AreaLightTemplate_ParseColor(fileIter, &template->diffuseReflectColor);
        fileIter = AreaLightTemplate_ParseColor(fileIter, &template->ambientReflectColor);
        fileIter = AreaLightTemplate_ParseColor(fileIter, &template->specularReflectColor);
        fileIter = AreaLightTemplate_ParseColor(fileIter, &template->emissionColor);
        fileIter = Ascii_CopyToTerminator(fileIter, lineBuffer, '\r');
    }

    Heap_FreeExplicit(HEAP_ID_FIELD1, fileBuffer);

    return templateCount;
}

static void AreaLightTemplate_Free(AreaLightTemplate **template)
{
    Heap_FreeExplicit(HEAP_ID_FIELD1, *template);
    *template = NULL;
}

static char *AreaLightTemplate_ParseLightAttrs(char *fileIter, GXRgb *lightColor, VecFx16 *lightVector)
{
    char lineBuffer[SCRATCH_BUFFER_SIZE];
    char partBuffer[SCRATCH_BUFFER_SIZE];

    fileIter = Ascii_CopyToTerminator(fileIter, lineBuffer, '\r');

    char *iter = lineBuffer;
    iter = Ascii_CopyToTerminator(iter, partBuffer, ',');
    u32 lightValid = Ascii_ConvertToInt(partBuffer);

    if (lightValid == TRUE) {
        int i;
        u16 lightColorParts[GX_COLOR_DIMS];
        s32 lightVectorParts[GX_VEC_FX_DIMS];

        for (i = 0; i < GX_COLOR_DIMS; i++) {
            iter = Ascii_CopyToTerminator(iter, partBuffer, ',');
            lightColorParts[i] = Ascii_ConvertToInt(partBuffer);
        }

        *lightColor = GX_RGB(lightColorParts[0], lightColorParts[1], lightColorParts[2]);

        for (i = 0; i < GX_VEC_FX_DIMS; i++) {
            iter = Ascii_CopyToTerminator(iter, partBuffer, ',');
            lightVectorParts[i] = Ascii_ConvertToInt(partBuffer);
        }

        lightVector->x = lightVectorParts[0];
        lightVector->y = lightVectorParts[1];
        lightVector->z = lightVectorParts[2];

        if (lightVector->x > FX16_ONE) {
            lightVector->x = FX16_ONE;
        }

        if (lightVector->x < -FX16_ONE) {
            lightVector->x = -FX16_ONE;
        }

        if (lightVector->y > FX16_ONE) {
            lightVector->y = FX16_ONE;
        }

        if (lightVector->y < -FX16_ONE) {
            lightVector->y = -FX16_ONE;
        }

        if (lightVector->z > FX16_ONE) {
            lightVector->z = FX16_ONE;
        }

        if (lightVector->z < -FX16_ONE) {
            lightVector->z = -FX16_ONE;
        }
    } else {
        *lightColor = INVALID_LIGHT_COLOR;
    }

    return fileIter;
}

static char *AreaLightTemplate_ParseColor(char *fileIter, GXRgb *color)
{
    char lineBuffer[SCRATCH_BUFFER_SIZE];
    char partBuffer[SCRATCH_BUFFER_SIZE];
    u16 colorParts[GX_COLOR_DIMS];

    fileIter = Ascii_CopyToTerminator(fileIter, lineBuffer, '\r');
    char *iter = lineBuffer;

    for (int i = 0; i < GX_COLOR_DIMS; i++) {
        iter = Ascii_CopyToTerminator(iter, partBuffer, ',');
        colorParts[i] = Ascii_ConvertToInt(partBuffer);
    }

    *color = GX_RGB(colorParts[0], colorParts[1], colorParts[2]);

    return fileIter;
}
