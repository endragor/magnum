#pragma once

#include <foundation/api_types.h>

#define MAG_TT_TYPE__TERRAIN_COMPONENT "mag_terrain_component"
#define MAG_TT_TYPE_HASH__TERRAIN_COMPONENT TM_STATIC_HASH("mag_terrain_component", 0xa6988af44e888d7bULL)

#define MAG_TT_TYPE__TERRAIN_SETTINGS "mag_terrain_settings"
#define MAG_TT_TYPE_HASH__TERRAIN_SETTINGS TM_STATIC_HASH("mag_terrain_settings", 0xd211ef5b239a0f0fULL)

#define MAG_TT_TYPE__TERRAIN_MATERIAL "mag_terrain_material"
#define MAG_TT_TYPE_HASH__TERRAIN_MATERIAL TM_STATIC_HASH("mag_terrain_material", 0x1773fda193a888aULL)

enum {
    MAG_TT_PROP__TERRAIN_COMPONENT__VISIBILITY_FLAGS, //suboject_set
    MAG_TT_PROP__TERRAIN_COMPONENT__COLOR, // subobject [[TM_TT_TYPE__COLOR_RGB]]
    MAG_TT_PROP__TERRAIN_COMPONENT__SETTINGS, // subobject [[MAG_TT_TYPE__TERRAIN_SETTINGS]]
};

enum {
    MAG_TT_PROP__TERRAIN_SETTINGS__MATERIALS, // subobject_set [[MAG_TT_TYPE__TERRAIN_MATERIAL]]
};

enum {
    MAG_TT_PROP__TERRAIN_MATERIAL__ORDER, // double
    MAG_TT_PROP__TERRAIN_MATERIAL__TEXTURES, // reference [[TM_TT_TYPE__CREATION_GRAPH]]
    MAG_TT_PROP__TERRAIN_MATERIAL__ALLOW_FROM_TOP, // bool
    MAG_TT_PROP__TERRAIN_MATERIAL__ALLOW_FROM_SIDES, // bool
    MAG_TT_PROP__TERRAIN_MATERIAL__PHYSICS_MATERIAL, // reference [[TM_TT_TYPE__PHYSICS_MATERIAL]]
    MAG_TT_PROP__TERRAIN_MATERIAL__PHYSICS_COLLISION, // reference [[TM_TT_TYPE__PHYSICS_COLLISION]]
};

// Manager for terrain engine. Get it using [[tm_entity_api->component_manager()]].
typedef struct mag_terrain_component_manager_o mag_terrain_component_manager_o;

typedef enum mag_terrain_op_type_t {
    TERRAIN_OP_UNION,
    TERRAIN_OP_SUBTRACT,
} mag_terrain_op_type_t;

typedef enum mag_terrain_op_primitive_t {
    TERRAIN_OP_SPHERE,
} mag_terrain_op_primitive_t;

struct mag_terrain_api
{
    // ray_dir is expected to be normalized.
    bool (*cast_ray)(mag_terrain_component_manager_o *man, tm_vec3_t ray_origin, tm_vec3_t ray_dir, float max_distance, float *hit_distance);

    void (*apply_operation)(mag_terrain_component_manager_o *man, mag_terrain_op_type_t type, mag_terrain_op_primitive_t primitive, tm_vec3_t pos, tm_vec3_t size);
};

#define mag_terrain_api_version TM_VERSION(1, 0, 0)

#define MAG_ENGINE__TERRAIN TM_STATIC_HASH("MAG_ENGINE__TERRAIN", 0x83fc814389cd9371ULL)