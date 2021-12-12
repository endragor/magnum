#pragma once

#include <foundation/api_types.h>

#define MAG_TT_TYPE__TERRAIN_COMPONENT "mag_terrain_component"
#define MAG_TT_TYPE_HASH__TERRAIN_COMPONENT TM_STATIC_HASH("mag_terrain_component", 0xa6988af44e888d7bULL)

enum {
    MAG_TT_PROP__TERRAIN_COMPONENT__VISIBILITY_FLAGS, //suboject_set
    MAG_TT_PROP__TERRAIN_COMPONENT__COLOR, // subobject [[TM_TT_TYPE__COLOR_RGB]]
};

#define MAG_ENGINE__TERRAIN TM_STATIC_HASH("MAG_ENGINE__TERRAIN", 0x83fc814389cd9371ULL)