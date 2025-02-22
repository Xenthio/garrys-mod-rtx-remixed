#include "entity_manager.hpp"
#include "mathlib/vector.h"
#include "mathlib/mathlib.h"
#include "vstdlib/random.h"
#include <algorithm>
#include <sstream>

using namespace GarrysMod::Lua;

namespace EntityManager {

// Initialize static members
std::vector<Light> cachedLights;
std::random_device rd;
std::mt19937 rng(rd());

// Helper function to parse color string "r g b a"
Vector ParseColorString(const char* colorStr) {
    std::istringstream iss(colorStr);
    float r, g, b, a = 200.0f;
    
    iss >> r >> g >> b;
    if (!iss.eof()) {
        iss >> a;
    }

    // Scale by intensity similar to Lua version
    float scale = a / 60000.0f;
    return Vector(r * scale, g * scale, b * scale);
}

void ShuffleLights() {
    std::shuffle(cachedLights.begin(), cachedLights.end(), rng);
}

void GetRandomLights(int count, std::vector<Light>& outLights) {
    outLights.clear();
    if (cachedLights.empty() || count <= 0) return;

    // Shuffle the lights if we need to
    ShuffleLights();

    // Get up to count lights
    int numLights = std::min(static_cast<size_t>(count), cachedLights.size());
    outLights.insert(outLights.end(), cachedLights.begin(), cachedLights.begin() + numLights);
}

void AngleVectorsRadians(const QAngle& angles, Vector* forward, Vector* right, Vector* up) {
    float sr, sp, sy, cr, cp, cy;

    sp = sin(angles.x * M_PI / 180.0f);
    cp = cos(angles.x * M_PI / 180.0f);
    sy = sin(angles.y * M_PI / 180.0f);
    cy = cos(angles.y * M_PI / 180.0f);
    sr = sin(angles.z * M_PI / 180.0f);
    cr = cos(angles.z * M_PI / 180.0f);

    if (forward) {
        forward->x = cp * cy;
        forward->y = cp * sy;
        forward->z = -sp;
    }

    if (right) {
        right->x = (-1 * sr * sp * cy + -1 * cr * -sy);
        right->y = (-1 * sr * sp * sy + -1 * cr * cy);
        right->z = -1 * sr * cp;
    }

    if (up) {
        up->x = (cr * sp * cy + -sr * -sy);
        up->y = (cr * sp * sy + -sr * cy);
        up->z = cr * cp;
    }
}

LUA_FUNCTION(CreateOptimizedMeshBatch_Native) {
    LUA->CheckType(1, Type::TABLE);  // vertices
    LUA->CheckType(2, Type::TABLE);  // normals
    LUA->CheckType(3, Type::TABLE);  // uvs
    uint32_t maxVertices = LUA->CheckNumber(4);

    std::vector<Vector> vertices;
    std::vector<Vector> normals;
    std::vector<BatchedMesh::UV> uvs;

    // Parse vertices table
    LUA->PushNil();
    while (LUA->Next(1) != 0) {
        if (LUA->IsType(-1, Type::Vector)) {
            Vector* v = LUA->GetUserType<Vector>(-1, Type::Vector);
            vertices.push_back(*v);
        }
        LUA->Pop();
    }

    // Parse normals table
    LUA->PushNil();
    while (LUA->Next(2) != 0) {
        if (LUA->IsType(-1, Type::Vector)) {
            Vector* n = LUA->GetUserType<Vector>(-1, Type::Vector);
            normals.push_back(*n);
        }
        LUA->Pop();
    }

    // Parse UVs table
    LUA->PushNil();
    while (LUA->Next(3) != 0) {
        if (LUA->IsType(-1, Type::Vector)) {
            Vector* uv = LUA->GetUserType<Vector>(-1, Type::Vector);
            uvs.push_back({uv->x, uv->y});  // Only use x,y for UVs
        }
        LUA->Pop();
    }

    BatchedMesh result = CreateOptimizedMeshBatch(vertices, normals, uvs, maxVertices);

    // Create return table with the same structure as before
    LUA->CreateTable();
    
    // Add vertices
    LUA->CreateTable();
    for (size_t i = 0; i < result.positions.size(); i++) {
        LUA->PushNumber(i + 1);
        Vector* v = new Vector(result.positions[i]);
        LUA->PushUserType(v, Type::Vector);
        LUA->SetTable(-3);
    }
    LUA->SetField(-2, "vertices");

    // Add normals
    LUA->CreateTable();
    for (size_t i = 0; i < result.normals.size(); i++) {
        LUA->PushNumber(i + 1);
        Vector* n = new Vector(result.normals[i]);
        LUA->PushUserType(n, Type::Vector);
        LUA->SetTable(-3);
    }
    LUA->SetField(-2, "normals");

    // Add UVs
    LUA->CreateTable();
    for (size_t i = 0; i < result.uvs.size(); i++) {
        LUA->PushNumber(i + 1);
        Vector* uv = new Vector(result.uvs[i].u, result.uvs[i].v, 0);
        LUA->PushUserType(uv, Type::Vector);
        LUA->SetTable(-3);
    }
    LUA->SetField(-2, "uvs");

    return 1;
}

BatchedMesh CreateOptimizedMeshBatch(const std::vector<Vector>& vertices,
                                   const std::vector<Vector>& normals,
                                   const std::vector<BatchedMesh::UV>& uvs,
                                   uint32_t maxVertices) {
    BatchedMesh result;
    result.vertexCount = 0;

    // Pre-allocate for efficiency
    result.positions.reserve(maxVertices);
    result.normals.reserve(maxVertices);
    result.uvs.reserve(maxVertices);

    // Process vertices in triangles
    for (size_t i = 0; i < vertices.size() && result.vertexCount < maxVertices; i += 3) {
        // Validate we have a complete triangle
        if (i + 2 >= vertices.size()) break;

        // Add vertices
        for (size_t j = 0; j < 3; j++) {
            result.positions.push_back(vertices[i + j]);
            if (i + j < normals.size()) {
                result.normals.push_back(normals[i + j]);
            }
            if (i + j < uvs.size()) {
                result.uvs.push_back(uvs[i + j]);
            }
            result.vertexCount++;
        }
    }

    return result;
}

LUA_FUNCTION(ProcessRegionBatch_Native) {
    LUA->CheckType(1, Type::TABLE);  // vertices
    LUA->CheckType(2, Type::Vector); // player position
    float threshold = LUA->CheckNumber(3);

    Vector* playerPos = LUA->GetUserType<Vector>(2, Type::Vector);
    std::vector<Vector> vertices;  // Changed to Source Vector

    // Parse vertices table
    LUA->PushNil();
    while (LUA->Next(1) != 0) {
        if (LUA->IsType(-1, Type::Vector)) {
            Vector* v = LUA->GetUserType<Vector>(-1, Type::Vector);
            vertices.push_back(*v);  // Copy the vector
        }
        LUA->Pop();
    }

    bool result = ProcessRegionBatch(vertices, *playerPos, threshold);
    LUA->PushBool(result);

    return 1;
}

bool ProcessRegionBatch(const std::vector<Vector>& vertices, 
                       const Vector& playerPos,
                       float threshold) {
    if (vertices.empty()) return false;

    // Calculate region bounds
    Vector mins(FLT_MAX, FLT_MAX, FLT_MAX);
    Vector maxs(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    for (const auto& vertex : vertices) {
        mins.x = std::min(mins.x, vertex.x);
        mins.y = std::min(mins.y, vertex.y);
        mins.z = std::min(mins.z, vertex.z);
        maxs.x = std::max(maxs.x, vertex.x);
        maxs.y = std::max(maxs.y, vertex.y);
        maxs.z = std::max(maxs.z, vertex.z);
    }

    // Create expanded bounds
    Vector expandedMins(
        mins.x - threshold,
        mins.y - threshold,
        mins.z - threshold
    );
    
    Vector expandedMaxs(
        maxs.x + threshold,
        maxs.y + threshold,
        maxs.z + threshold
    );

    // Check if player is within expanded bounds
    return (playerPos.x >= expandedMins.x && playerPos.x <= expandedMaxs.x &&
            playerPos.y >= expandedMins.y && playerPos.y <= expandedMaxs.y &&
            playerPos.z >= expandedMins.z && playerPos.z <= expandedMaxs.z);
}

LUA_FUNCTION(CalculateSpecialEntityBounds_Native) {
    LUA->CheckType(1, Type::Entity);
    LUA->CheckNumber(2);  // size

    float size = LUA->GetNumber(2);

    // Get entity angles
    LUA->GetField(1, "GetAngles");
    LUA->Push(1);
    LUA->Call(1, 1);
    QAngle* angles = LUA->GetUserType<QAngle>(-1, Type::Angle);

    // Calculate forward, right, up vectors using our implementation
    Vector forward, right, up;
    AngleVectorsRadians(*angles, &forward, &right, &up);

    // Calculate bounds
    Vector scaledForward = forward * (size * 2);  // Double size in rotation direction
    Vector scaledRight = right * size;
    Vector scaledUp = up * size;

    Vector customMins(
        -std::abs(scaledForward.x) - std::abs(scaledRight.x) - std::abs(scaledUp.x),
        -std::abs(scaledForward.y) - std::abs(scaledRight.y) - std::abs(scaledUp.y),
        -std::abs(scaledForward.z) - std::abs(scaledRight.z) - std::abs(scaledUp.z)
    );

    Vector customMaxs(
        std::abs(scaledForward.x) + std::abs(scaledRight.x) + std::abs(scaledUp.x),
        std::abs(scaledForward.y) + std::abs(scaledRight.y) + std::abs(scaledUp.y),
        std::abs(scaledForward.z) + std::abs(scaledRight.z) + std::abs(scaledUp.z)
    );

    // Set the calculated bounds
    LUA->GetField(1, "SetRenderBounds");
    LUA->Push(1);
    LUA->PushVector(customMins);
    LUA->PushVector(customMaxs);
    LUA->Call(3, 0);

    LUA->Pop();  // Pop angles
    return 0;
}

LUA_FUNCTION(FilterEntitiesByDistance_Native) {
    LUA->CheckType(1, Type::TABLE);  // entities
    LUA->CheckType(2, Type::Vector);  // origin
    LUA->CheckNumber(3);  // maxDistance

    Vector* origin = LUA->GetUserType<Vector>(2, Type::Vector);
    float maxDistance = LUA->GetNumber(3);
    float maxDistSqr = maxDistance * maxDistance;

    // Create result table
    LUA->CreateTable();
    int resultTable = LUA->Top();
    int index = 1;

    // Iterate input table
    LUA->PushNil();
    while (LUA->Next(1) != 0) {
        if (LUA->IsType(-1, Type::Entity)) {
            // Get entity position
            LUA->GetField(-1, "GetPos");
            LUA->Push(-2);
            LUA->Call(1, 1);
            Vector* pos = LUA->GetUserType<Vector>(-1, Type::Vector);

            // Calculate distance
            float dx = pos->x - origin->x;
            float dy = pos->y - origin->y;
            float dz = pos->z - origin->z;
            float distSqr = dx*dx + dy*dy + dz*dz;

            if (distSqr <= maxDistSqr) {
                LUA->PushNumber(index++);
                LUA->Push(-2);  // Push the entity
                LUA->SetTable(resultTable);
            }

            LUA->Pop();  // Pop position
        }
        LUA->Pop();  // Pop value, keep key for next iteration
    }

    return 1;  // Return the filtered table
}

LUA_FUNCTION(BatchUpdateEntityBounds_Native) {
    LUA->CheckType(1, Type::TABLE);
    LUA->CheckType(2, Type::Vector);
    LUA->CheckType(3, Type::Vector);

    Vector* mins = LUA->GetUserType<Vector>(2, Type::Vector);
    Vector* maxs = LUA->GetUserType<Vector>(3, Type::Vector);

    RTXMath::Vector3 rtxMins = {mins->x, mins->y, mins->z};
    RTXMath::Vector3 rtxMaxs = {maxs->x, maxs->y, maxs->z};

    // Process entity table
    LUA->PushNil();
    while (LUA->Next(1) != 0) {
        if (LUA->IsType(-1, Type::Entity)) {
            // Get entity position
            LUA->GetField(-1, "GetPos");
            LUA->Push(-2);
            LUA->Call(1, 1);
            Vector* pos = LUA->GetUserType<Vector>(-1, Type::Vector);

            if (RTXMath::IsWithinBounds({pos->x, pos->y, pos->z}, rtxMins, rtxMaxs)) {
                // Set render bounds
                LUA->GetField(-2, "SetRenderBounds");
                LUA->Push(-3);
                LUA->Push(2);  // mins
                LUA->Push(3);  // maxs
                LUA->Call(3, 0);
            }

            LUA->Pop();  // Pop position
        }
        LUA->Pop();  // Pop value, keep key for next iteration
    }

    return 0;
}

LUA_FUNCTION(UpdateLightCache_Native) {
    LUA->CheckType(1, Type::TABLE); // lights table

    cachedLights.clear();
    
    // Iterate input table
    LUA->PushNil();
    while (LUA->Next(1) != 0) {
        if (LUA->IsType(-1, Type::Table)) {
            Light light = {};
            
            // Get class name to determine light type
            LUA->GetField(-1, "classname");
            const char* className = LUA->GetString(-1);
            LUA->Pop();

            // Skip spotlight and disabled lights
            if (strcmp(className, "point_spotlight") == 0) {
                LUA->Pop();
                continue;
            }

            // Check spawnflags
            LUA->GetField(-1, "spawnflags");
            int spawnFlags = LUA->GetNumber(-1);
            LUA->Pop();

            if (spawnFlags == 1) { // Light starts off
                LUA->Pop();
                continue;
            }

            // Set light type
            if (strcmp(className, "light") == 0) light.type = LIGHT_POINT;
            else if (strcmp(className, "light_environment") == 0) light.type = LIGHT_DIRECTIONAL;
            else if (strcmp(className, "light_spot") == 0) light.type = LIGHT_SPOT;
            else if (strcmp(className, "light_dynamic") == 0) light.type = LIGHT_POINT;
            else {
                LUA->Pop();
                continue;
            }

            // Get position
            LUA->GetField(-1, "origin");
            if (LUA->IsType(-1, Type::Vector)) {
                Vector* pos = LUA->GetUserType<Vector>(-1, Type::Vector);
                light.position = *pos;
            }
            LUA->Pop();

            // Get color
            LUA->GetField(-1, "_light");
            if (LUA->IsType(-1, Type::String)) {
                light.color = ParseColorString(LUA->GetString(-1));
            }
            LUA->Pop();

            // Get angles and direction
            LUA->GetField(-1, "angles");
            QAngle* angles = nullptr;
            if (LUA->IsType(-1, Type::Angle)) {
                angles = LUA->GetUserType<QAngle>(-1, Type::Angle);
            }
            LUA->Pop();

            LUA->GetField(-1, "pitch");
            float pitch = LUA->GetNumber(-1);
            LUA->Pop();

            // Calculate direction
            if (angles) {
                QAngle finalAngle;
                if (light.type == LIGHT_DIRECTIONAL) {
                    finalAngle.Init(pitch * -1, angles->y, angles->z); // Use y and z instead of r
                } else {
                    finalAngle.Init(angles->x != 0 ? pitch * -1 : -90, angles->y, angles->z);
                }

                Vector forward, right, up;
                AngleVectorsRadians(*angles, &forward, &right, &up);
                light.direction = forward;
            }

            // Get other properties
            LUA->GetField(-1, "_inner_cone");
            light.innerAngle = LUA->GetNumber(-1) * 2;  // Double the angle as per Lua
            LUA->Pop();

            LUA->GetField(-1, "_cone");
            light.outerAngle = LUA->GetNumber(-1) * 2;  // Double the angle as per Lua
            LUA->Pop();

            LUA->GetField(-1, "_exponent");
            light.angularFalloff = LUA->GetNumber(-1);
            LUA->Pop();

            LUA->GetField(-1, "_quadratic_attn");
            light.quadraticFalloff = LUA->GetNumber(-1);
            LUA->Pop();

            LUA->GetField(-1, "_linear_attn");
            light.linearFalloff = LUA->GetNumber(-1);
            LUA->Pop();

            LUA->GetField(-1, "_constant_attn");
            light.constantFalloff = LUA->GetNumber(-1);
            LUA->Pop();

            LUA->GetField(-1, "_fifty_percent_distance");
            light.fiftyPercentDistance = LUA->GetNumber(-1);
            LUA->Pop();

            LUA->GetField(-1, "_zero_percent_distance");
            light.zeroPercentDistance = LUA->GetNumber(-1);
            LUA->Pop();

            // Get range
            LUA->GetField(-1, "distance");
            light.range = LUA->GetNumber(-1);
            if (light.range <= 0) light.range = 512;
            LUA->Pop();

            // Set default angles if not specified
            if (light.innerAngle == 0) light.innerAngle = 30 * 2;
            if (light.outerAngle == 0) light.outerAngle = 45 * 2;

            // Special handling for environment lights
            if (light.type == LIGHT_DIRECTIONAL) {
                light.position = Vector(0, 0, 0);
            }

            cachedLights.push_back(light);
        }
        LUA->Pop();
    }

    return 0;
}

LUA_FUNCTION(GetRandomLights_Native) {
    LUA->CheckNumber(1); // count

    int count = (int)LUA->GetNumber(1);
    
    // Get random lights
    std::vector<Light> randomLights;
    GetRandomLights(count, randomLights);

    // Create result table
    LUA->CreateTable();

    // Convert to Lua table
    for (size_t i = 0; i < randomLights.size(); i++) {
        const Light& light = randomLights[i];

        LUA->PushNumber(i + 1);
        LUA->CreateTable();

        // Push all light properties
        int luaLightType;
        switch (light.type) {
            case LIGHT_POINT: luaLightType = 0; break;
            case LIGHT_SPOT: luaLightType = 1; break;
            case LIGHT_DIRECTIONAL: luaLightType = 2; break;
            default: luaLightType = 0; break;
        }
        LUA->PushNumber(luaLightType);
        LUA->SetField(-2, "type");

        LUA->PushVector(light.color);
        LUA->SetField(-2, "color");

        LUA->PushVector(light.position);
        LUA->SetField(-2, "pos");

        LUA->PushVector(light.direction);
        LUA->SetField(-2, "dir");

        LUA->PushNumber(light.range);
        LUA->SetField(-2, "range");

        LUA->PushNumber(light.innerAngle);
        LUA->SetField(-2, "innerAngle");

        LUA->PushNumber(light.outerAngle);
        LUA->SetField(-2, "outerAngle");

        LUA->PushNumber(light.angularFalloff);
        LUA->SetField(-2, "angularFalloff");

        LUA->PushNumber(light.quadraticFalloff);
        LUA->SetField(-2, "quadraticFalloff");

        LUA->PushNumber(light.linearFalloff);
        LUA->SetField(-2, "linearFalloff");

        LUA->PushNumber(light.constantFalloff);
        LUA->SetField(-2, "constantFalloff");

        if (light.fiftyPercentDistance > 0) {
            LUA->PushNumber(light.fiftyPercentDistance);
            LUA->SetField(-2, "fiftyPercentDistance");
        }

        if (light.zeroPercentDistance > 0) {
            LUA->PushNumber(light.zeroPercentDistance);
            LUA->SetField(-2, "zeroPercentDistance");
        }

        LUA->SetTable(-3);
    }

    return 1;
}

void Initialize(ILuaBase* LUA) {
    LUA->CreateTable();

    // Add existing functions
    LUA->PushCFunction(BatchUpdateEntityBounds_Native);
    LUA->SetField(-2, "BatchUpdateEntityBounds");

    LUA->PushCFunction(CalculateSpecialEntityBounds_Native);
    LUA->SetField(-2, "CalculateSpecialEntityBounds");

    LUA->PushCFunction(FilterEntitiesByDistance_Native);
    LUA->SetField(-2, "FilterEntitiesByDistance");

    LUA->PushCFunction(UpdateLightCache_Native);
    LUA->SetField(-2, "UpdateLightCache");

    LUA->PushCFunction(GetRandomLights_Native);
    LUA->SetField(-2, "GetRandomLights");

    LUA->PushCFunction(CreateOptimizedMeshBatch_Native);
    LUA->SetField(-2, "CreateOptimizedMeshBatch");

    LUA->PushCFunction(ProcessRegionBatch_Native);
    LUA->SetField(-2, "ProcessRegionBatch");

    LUA->SetField(-2, "EntityManager");
}

} // namespace EntityManager