#pragma once

#include <foundation/api_types.h>

#define MAG_TT_TYPE__TERRAIN_COMPONENT "mag_terrain_component"
#define MAG_TT_TYPE_HASH__TERRAIN_COMPONENT TM_STATIC_HASH("mag_terrain_component", 0xa6988af44e888d7bULL)

enum {
    MAG_TT_PROP__TERRAIN_COMPONENT__VISIBILITY_FLAGS, //suboject_set
    MAG_TT_PROP__TERRAIN_COMPONENT__COLOR, // subobject [[TM_TT_TYPE__COLOR_RGB]]
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