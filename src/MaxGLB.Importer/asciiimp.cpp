#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <locale>
#include <locale.h>

#include "../../third_party/cgltf/cgltf.h"

#include <max.h>
#include <impexp.h>
#include <MeshNormalSpec.h>
#include <stdmat.h>
#include <iparamb2.h>
#include <istdplug.h>
#include <maxheapdirect.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")

HINSTANCE hInstance = NULL;

// Import is single-threaded in 3ds Max 2016. Texture files use the stable
// cgltf image index, so shared images and repeated imports reuse the same
// extracted files instead of creating object-based duplicates.
static int g_importTextureSetIndex = 0;
static const cgltf_data* g_activeImportData = NULL;
static std::map<const cgltf_image*, BOOL> g_splitOrmImagesThisImport;
static std::map<const cgltf_image*, BOOL> g_unchangedImagesThisImport;


struct MaxGLBImportSettings
{
    // Row-major 3x3 rotation matrix applied after the official
    // glTF Y-up -> 3ds Max Z-up conversion.
    float rotationMatrix[9];

    BOOL importTextures;
    BOOL addParentNode;
    BOOL normalizeSize;
    float normalizeTargetSize;
    float appliedUniformScale;
    BOOL importAnimation;

    MaxGLBImportSettings()
        : importTextures(TRUE)
        , addParentNode(TRUE)
        , normalizeSize(FALSE)
        , normalizeTargetSize(1.0f)
        , appliedUniformScale(1.0f)
        , importAnimation(FALSE)
    {
        rotationMatrix[0] = 1.0f;
        rotationMatrix[1] = 0.0f;
        rotationMatrix[2] = 0.0f;

        rotationMatrix[3] = 0.0f;
        rotationMatrix[4] = 1.0f;
        rotationMatrix[5] = 0.0f;

        rotationMatrix[6] = 0.0f;
        rotationMatrix[7] = 0.0f;
        rotationMatrix[8] = 1.0f;
    }
};


enum MaxGLBAlphaMode
{
    MAXGLB_ALPHA_AUTO = 0,
    MAXGLB_ALPHA_OPAQUE = 1,
    MAXGLB_ALPHA_MASK = 2,
    MAXGLB_ALPHA_BLEND = 3
};


struct MaxGLBExportSettings
{
    float scaleFactor;
    BOOL normalizeSize;
    float normalizeTargetSize;
    float appliedUniformScale;

    BOOL exportMaterials;
    BOOL exportTextures;

    int alphaMode;
    float alphaCutoff;

    BOOL showSummary;
    BOOL showInExplorer;
    BOOL exportAnimation;

    MaxGLBExportSettings()
        : scaleFactor(1.0f)
        , normalizeSize(FALSE)
        , normalizeTargetSize(1.0f)
        , appliedUniformScale(1.0f)
        , exportMaterials(TRUE)
        , exportTextures(TRUE)
        , alphaMode(MAXGLB_ALPHA_AUTO)
        , alphaCutoff(0.5f)
        , showSummary(TRUE)
        , showInExplorer(FALSE)
        , exportAnimation(FALSE)
    {
    }
};


// Import is single-threaded in 3ds Max 2016. These settings are active only
// while one file is being imported. The last accepted values are remembered
// for the next manually opened GLB.
static MaxGLBImportSettings g_activeImportSettings;
static MaxGLBImportSettings g_lastImportSettings;

static MaxGLBExportSettings g_lastExportSettings;


struct MaxGLBProgressState
{
    Interface* maxInterface;
    BOOL active;
    BOOL cancelled;
    int lastPercent;

    MaxGLBProgressState()
        : maxInterface(NULL)
        , active(FALSE)
        , cancelled(FALSE)
        , lastPercent(-1)
    {
    }
};


static MaxGLBProgressState g_maxGLBProgress;
static int g_activeImportPrimitiveCount = 1;


// 3ds Max 2016's four-argument ProgressStart implementation dereferences
// the worker callback even though newer SDK documentation allows NULL.
// A tiny valid callback prevents the null-function-pointer crash while the
// importer/exporter continues to perform the work synchronously and calls
// ProgressUpdate itself.
static DWORD WINAPI MaxGLBProgressWorkerStub(
    LPVOID argument)
{
    UNREFERENCED_PARAMETER(argument);
    return 0;
}


static int g_exportProgressTotalNodes = 1;
static int g_exportProgressProcessedNodes = 0;
static int g_exportNextNodeGroupIndex = 0;


static void BeginMaxGLBProgress(
    Interface* maxInterface,
    const TCHAR* taskName)
{
    g_maxGLBProgress.maxInterface =
        maxInterface;

    g_maxGLBProgress.active =
        FALSE;

    g_maxGLBProgress.cancelled =
        FALSE;

    g_maxGLBProgress.lastPercent =
        -1;

    if (maxInterface == NULL)
    {
        return;
    }

    maxInterface->SetCancel(FALSE);

    const BOOL progressStarted =
        maxInterface->ProgressStart(
            const_cast<TCHAR*>(taskName),
            TRUE,
            MaxGLBProgressWorkerStub,
            NULL);

    g_maxGLBProgress.active =
        progressStarted;
}


static BOOL UpdateMaxGLBProgress(
    int percent,
    const TCHAR* stepName)
{
    if (percent < 0)
    {
        percent = 0;
    }
    else if (percent > 100)
    {
        percent = 100;
    }

    Interface* maxInterface =
        g_maxGLBProgress.maxInterface;

    if (maxInterface == NULL)
    {
        return TRUE;
    }

    if (!g_maxGLBProgress.active)
    {
        return TRUE;
    }

    if (percent != g_maxGLBProgress.lastPercent ||
        stepName != NULL)
    {
        maxInterface->ProgressUpdate(
            percent,
            TRUE,
            const_cast<TCHAR*>(stepName));

        g_maxGLBProgress.lastPercent =
            percent;
    }

    if (maxInterface->GetCancel())
    {
        g_maxGLBProgress.cancelled =
            TRUE;

        return FALSE;
    }

    return TRUE;
}


static BOOL ContinueMaxGLBOperation(
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (UpdateMaxGLBProgress(
            g_maxGLBProgress.lastPercent < 0
                ? 0
                : g_maxGLBProgress.lastPercent,
            NULL))
    {
        return TRUE;
    }

    if (errorMessage != NULL &&
        errorMessageCount > 0)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Operation cancelled by the user."));
    }

    return FALSE;
}


static BOOL MaxGLBOperationWasCancelled()
{
    return g_maxGLBProgress.cancelled;
}


static void EndMaxGLBProgress()
{
    Interface* maxInterface =
        g_maxGLBProgress.maxInterface;

    if (maxInterface != NULL)
    {
        if (g_maxGLBProgress.active)
        {
            maxInterface->ProgressEnd();
        }

        maxInterface->SetCancel(FALSE);
    }

    g_maxGLBProgress =
        MaxGLBProgressState();
}


static int CountSceneNodesRecursive(
    INode* parentNode)
{
    if (parentNode == NULL)
    {
        return 0;
    }

    int result = 0;

    const int childCount =
        parentNode->NumberOfChildren();

    for (int childIndex = 0;
         childIndex < childCount;
         ++childIndex)
    {
        INode* childNode =
            parentNode->GetChildNode(
                childIndex);

        if (childNode == NULL)
        {
            continue;
        }

        ++result;

        result +=
            CountSceneNodesRecursive(
                childNode);
    }

    return result;
}

// Eigene, dauerhafte Class-ID fuer MaxGLB2016.
// Diese Werte spaeter nicht mehr veraendern.
#define MAXGLB_IMPORTER_CLASS_ID \
    Class_ID(0x7a4e21c3, 0x4f1b6d92)

// Separate permanent Class-ID for the SceneExport plug-in.
// Never change this after the public release.
#define MAXGLB_EXPORTER_CLASS_ID \
    Class_ID(0x2b6f4d81, 0x71a35ce4)


static const DWORD MAXGLB_ALPHA_METADATA_MAGIC =
    0x414C5048u; // "ALPH"

static const DWORD MAXGLB_ALPHA_METADATA_VERSION =
    1u;

static const DWORD MAXGLB_ALPHA_METADATA_SUB_ID =
    0x4D474C42u; // "MGLB"

static const DWORD MAXGLB_ALPHA_FLAG_OPACITY_FROM_BASE_ALPHA =
    0x00000001u;


struct MaxGLBAlphaMetadata
{
    DWORD magic;
    DWORD version;
    int mode;
    float cutoff;
    DWORD flags;
};


static float ClampMaxGLBAlphaCutoff(
    float value)
{
    if (value < 0.0f)
    {
        return 0.0f;
    }

    if (value > 1.0f)
    {
        return 1.0f;
    }

    return value;
}


static void StoreMaxGLBAlphaMetadata(
    Mtl* material,
    int alphaMode,
    float alphaCutoff,
    DWORD flags)
{
    if (material == NULL)
    {
        return;
    }

    material->RemoveAppDataChunk(
        MAXGLB_IMPORTER_CLASS_ID,
        MATERIAL_CLASS_ID,
        MAXGLB_ALPHA_METADATA_SUB_ID);

    MaxGLBAlphaMetadata* metadata =
        static_cast<MaxGLBAlphaMetadata*>(
            MAX_malloc(
                sizeof(MaxGLBAlphaMetadata)));

    if (metadata == NULL)
    {
        return;
    }

    metadata->magic =
        MAXGLB_ALPHA_METADATA_MAGIC;

    metadata->version =
        MAXGLB_ALPHA_METADATA_VERSION;

    metadata->mode =
        alphaMode;

    metadata->cutoff =
        ClampMaxGLBAlphaCutoff(
            alphaCutoff);

    metadata->flags =
        flags;

    material->AddAppDataChunk(
        MAXGLB_IMPORTER_CLASS_ID,
        MATERIAL_CLASS_ID,
        MAXGLB_ALPHA_METADATA_SUB_ID,
        static_cast<DWORD>(
            sizeof(MaxGLBAlphaMetadata)),
        metadata);
}


static BOOL ReadMaxGLBAlphaMetadata(
    Mtl* material,
    int* outAlphaMode,
    float* outAlphaCutoff,
    DWORD* outFlags)
{
    if (outAlphaMode != NULL)
    {
        *outAlphaMode =
            MAXGLB_ALPHA_OPAQUE;
    }

    if (outAlphaCutoff != NULL)
    {
        *outAlphaCutoff =
            0.5f;
    }

    if (outFlags != NULL)
    {
        *outFlags = 0;
    }

    if (material == NULL)
    {
        return FALSE;
    }

    AppDataChunk* chunk =
        material->GetAppDataChunk(
            MAXGLB_IMPORTER_CLASS_ID,
            MATERIAL_CLASS_ID,
            MAXGLB_ALPHA_METADATA_SUB_ID);

    if (chunk == NULL ||
        chunk->data == NULL ||
        chunk->length <
            sizeof(MaxGLBAlphaMetadata))
    {
        return FALSE;
    }

    const MaxGLBAlphaMetadata* metadata =
        static_cast<const MaxGLBAlphaMetadata*>(
            chunk->data);

    if (metadata->magic !=
            MAXGLB_ALPHA_METADATA_MAGIC ||
        metadata->version !=
            MAXGLB_ALPHA_METADATA_VERSION ||
        metadata->mode <
            MAXGLB_ALPHA_OPAQUE ||
        metadata->mode >
            MAXGLB_ALPHA_BLEND)
    {
        return FALSE;
    }

    if (outAlphaMode != NULL)
    {
        *outAlphaMode =
            metadata->mode;
    }

    if (outAlphaCutoff != NULL)
    {
        *outAlphaCutoff =
            ClampMaxGLBAlphaCutoff(
                metadata->cutoff);
    }

    if (outFlags != NULL)
    {
        *outFlags =
            metadata->flags;
    }

    return TRUE;
}


static int ConvertCgltfAlphaMode(
    cgltf_alpha_mode mode)
{
    if (mode == cgltf_alpha_mode_mask)
    {
        return MAXGLB_ALPHA_MASK;
    }

    if (mode == cgltf_alpha_mode_blend)
    {
        return MAXGLB_ALPHA_BLEND;
    }

    return MAXGLB_ALPHA_OPAQUE;
}


static void ShowImportMessage(
    HWND parentWindow,
    const TCHAR* text,
    UINT icon)
{
    MessageBox(
        parentWindow,
        text,
        _T("MaxGLB2016"),
        MB_OK | icon);
}


// GLB 2.0 normally stores its geometry buffer in the embedded BIN chunk.
// cgltf_parse() exposes that chunk as data->bin. For this first importer
// milestone we support embedded GLB buffers only.
static BOOL AttachEmbeddedGlbBuffer(cgltf_data* data)
{
    if (data == NULL ||
        data->file_type != cgltf_file_type_glb ||
        data->bin == NULL ||
        data->bin_size == 0)
    {
        return FALSE;
    }

    for (cgltf_size bufferIndex = 0;
         bufferIndex < data->buffers_count;
         ++bufferIndex)
    {
        cgltf_buffer* buffer = &data->buffers[bufferIndex];

        if (buffer->uri == NULL)
        {
            if (buffer->size > data->bin_size)
            {
                return FALSE;
            }

            buffer->data = const_cast<void*>(data->bin);
            buffer->data_free_method = cgltf_data_free_method_none;
            return TRUE;
        }
    }

    return FALSE;
}


static cgltf_primitive* FindFirstTrianglePrimitive(
    cgltf_data* data,
    cgltf_mesh** outMesh)
{
    if (outMesh != NULL)
    {
        *outMesh = NULL;
    }

    if (data == NULL)
    {
        return NULL;
    }

    for (cgltf_size meshIndex = 0;
         meshIndex < data->meshes_count;
         ++meshIndex)
    {
        cgltf_mesh* mesh = &data->meshes[meshIndex];

        for (cgltf_size primitiveIndex = 0;
             primitiveIndex < mesh->primitives_count;
             ++primitiveIndex)
        {
            cgltf_primitive* primitive =
                &mesh->primitives[primitiveIndex];

            if (primitive->type != cgltf_primitive_type_triangles)
            {
                continue;
            }

            const cgltf_accessor* positions =
                cgltf_find_accessor(
                    primitive,
                    cgltf_attribute_type_position,
                    0);

            if (positions == NULL ||
                positions->type != cgltf_type_vec3 ||
                positions->count == 0)
            {
                continue;
            }

            if (outMesh != NULL)
            {
                *outMesh = mesh;
            }

            return primitive;
        }
    }

    return NULL;
}


static const cgltf_node* FindFirstNodeForMesh(
    const cgltf_data* data,
    const cgltf_mesh* mesh)
{
    if (data == NULL || mesh == NULL)
    {
        return NULL;
    }

    for (cgltf_size nodeIndex = 0;
         nodeIndex < data->nodes_count;
         ++nodeIndex)
    {
        const cgltf_node* node = &data->nodes[nodeIndex];

        if (node->mesh == mesh)
        {
            return node;
        }
    }

    return NULL;
}


// Find a node's parent by scanning the explicit children arrays. This is
// deliberately independent of cgltf_node::parent, so ancestor transforms are
// preserved even if a parsed file does not expose the parent link as expected.
static const cgltf_node* FindParentNode(
    const cgltf_data* data,
    const cgltf_node* child)
{
    if (data == NULL || child == NULL)
    {
        return NULL;
    }

    for (cgltf_size nodeIndex = 0;
         nodeIndex < data->nodes_count;
         ++nodeIndex)
    {
        const cgltf_node* candidateParent =
            &data->nodes[nodeIndex];

        for (cgltf_size childIndex = 0;
             childIndex < candidateParent->children_count;
             ++childIndex)
        {
            if (candidateParent->children[childIndex] == child)
            {
                return candidateParent;
            }
        }
    }

    return NULL;
}


static void SetIdentityGltfMatrix(cgltf_float* matrix)
{
    for (int index = 0; index < 16; ++index)
    {
        matrix[index] = 0.0f;
    }

    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[10] = 1.0f;
    matrix[15] = 1.0f;
}


// Column-major matrix multiplication: out = left * right.
static void MultiplyGltfMatrices(
    const cgltf_float* left,
    const cgltf_float* right,
    cgltf_float* outMatrix)
{
    cgltf_float result[16];

    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            result[column * 4 + row] =
                left[0 * 4 + row] * right[column * 4 + 0] +
                left[1 * 4 + row] * right[column * 4 + 1] +
                left[2 * 4 + row] * right[column * 4 + 2] +
                left[3 * 4 + row] * right[column * 4 + 3];
        }
    }

    for (int index = 0; index < 16; ++index)
    {
        outMatrix[index] = result[index];
    }
}


// Build the root-to-node matrix from the hierarchy stored in children[].
// This preserves transforms on empty parent nodes, which are common in GLB
// files exported from DCC applications.
static void BuildGltfWorldMatrix(
    const cgltf_data* data,
    const cgltf_node* node,
    cgltf_float* outMatrix)
{
    SetIdentityGltfMatrix(outMatrix);

    if (data == NULL || node == NULL)
    {
        return;
    }

    std::vector<const cgltf_node*> hierarchy;
    const cgltf_node* current = node;

    while (current != NULL)
    {
        hierarchy.push_back(current);
        current = FindParentNode(data, current);
    }

    for (std::vector<const cgltf_node*>::reverse_iterator iterator =
             hierarchy.rbegin();
         iterator != hierarchy.rend();
         ++iterator)
    {
        cgltf_float localMatrix[16];
        cgltf_node_transform_local(*iterator, localMatrix);
        MultiplyGltfMatrices(outMatrix, localMatrix, outMatrix);
    }
}


static Point3 ApplyImportRotationMatrix(
    const Point3& point,
    const float* matrix)
{
    if (matrix == NULL)
    {
        return point;
    }

    return Point3(
        matrix[0] * point.x +
        matrix[1] * point.y +
        matrix[2] * point.z,

        matrix[3] * point.x +
        matrix[4] * point.y +
        matrix[5] * point.z,

        matrix[6] * point.x +
        matrix[7] * point.y +
        matrix[8] * point.z);
}


static void MultiplyImportRotationMatrices(
    const float* left,
    const float* right,
    float* output)
{
    if (left == NULL ||
        right == NULL ||
        output == NULL)
    {
        return;
    }

    float result[9];

    for (int row = 0;
         row < 3;
         ++row)
    {
        for (int column = 0;
             column < 3;
             ++column)
        {
            result[row * 3 + column] =
                left[row * 3 + 0] *
                    right[0 * 3 + column] +
                left[row * 3 + 1] *
                    right[1 * 3 + column] +
                left[row * 3 + 2] *
                    right[2 * 3 + column];
        }
    }

    for (int elementIndex = 0;
         elementIndex < 9;
         ++elementIndex)
    {
        output[elementIndex] =
            result[elementIndex];
    }
}


static void ResetImportRotationMatrix(
    MaxGLBImportSettings* settings)
{
    if (settings == NULL)
    {
        return;
    }

    settings->rotationMatrix[0] = 1.0f;
    settings->rotationMatrix[1] = 0.0f;
    settings->rotationMatrix[2] = 0.0f;

    settings->rotationMatrix[3] = 0.0f;
    settings->rotationMatrix[4] = 1.0f;
    settings->rotationMatrix[5] = 0.0f;

    settings->rotationMatrix[6] = 0.0f;
    settings->rotationMatrix[7] = 0.0f;
    settings->rotationMatrix[8] = 1.0f;
}


static void ApplyImportQuarterTurn(
    MaxGLBImportSettings* settings,
    int axis,
    int quarterTurns)
{
    if (settings == NULL)
    {
        return;
    }

    const float radians =
        static_cast<float>(quarterTurns) *
        3.14159265358979323846f /
        2.0f;

    const float cosine =
        cosf(radians);

    const float sine =
        sinf(radians);

    float turn[9] =
    {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f
    };

    if (axis == 0)
    {
        // Max X: pitch up/down.
        turn[4] = cosine;
        turn[5] = -sine;
        turn[7] = sine;
        turn[8] = cosine;
    }
    else
    {
        // Max Z: turn left/right while retaining Z-up.
        turn[0] = cosine;
        turn[1] = -sine;
        turn[3] = sine;
        turn[4] = cosine;
    }

    // Pre-multiply: every click rotates the complete model around
    // the visible Max-world axes.
    MultiplyImportRotationMatrices(
        turn,
        settings->rotationMatrix,
        settings->rotationMatrix);
}


static Point3 ConvertGltfPositionToMax(
    const cgltf_float* position,
    const float* correctionMatrix)
{
    // Standards conversion: glTF is right-handed/Y-up, Max is Z-up.
    const Point3 maxPosition(
        static_cast<float>(position[0]),
        static_cast<float>(-position[2]),
        static_cast<float>(position[1]));

    return ApplyImportRotationMatrix(
        maxPosition,
        correctionMatrix);
}


static Point3 ConvertGltfNormalToMax(
    const cgltf_float* normal,
    const float* correctionMatrix)
{
    Point3 maxNormal(
        static_cast<float>(normal[0]),
        static_cast<float>(-normal[2]),
        static_cast<float>(normal[1]));

    maxNormal =
        ApplyImportRotationMatrix(
            maxNormal,
            correctionMatrix);

    const float lengthSquared =
        DotProd(
            maxNormal,
            maxNormal);

    if (lengthSquared > 1.0e-20f)
    {
        maxNormal *=
            1.0f /
            sqrtf(lengthSquared);
    }

    return maxNormal;
}


static Point3 ConvertGltfLocalPositionToMax(
    const cgltf_float* position)
{
    Point3 converted =
        ConvertGltfPositionToMax(
            position,
            g_activeImportSettings.rotationMatrix);

    converted *=
        g_activeImportSettings.appliedUniformScale;

    return converted;
}


static Point3 ConvertGltfLocalNormalToMax(
    const cgltf_float* normal)
{
    return ConvertGltfNormalToMax(
        normal,
        g_activeImportSettings.rotationMatrix);
}


static void TransformGltfPoint(
    const cgltf_float* matrix,
    cgltf_float x,
    cgltf_float y,
    cgltf_float z,
    cgltf_float* outPoint)
{
    outPoint[0] =
        matrix[0] * x +
        matrix[4] * y +
        matrix[8] * z +
        matrix[12];

    outPoint[1] =
        matrix[1] * x +
        matrix[5] * y +
        matrix[9] * z +
        matrix[13];

    outPoint[2] =
        matrix[2] * x +
        matrix[6] * y +
        matrix[10] * z +
        matrix[14];
}


static Matrix3 BuildMaxNodeTransform(
    const cgltf_node* sourceNode)
{
    Matrix3 maxTransform(1);

    if (sourceNode == NULL)
    {
        return maxTransform;
    }

    cgltf_float gltfWorld[16];
    cgltf_node_transform_world(
        sourceNode,
        gltfWorld);

    cgltf_float gltfOrigin[3];
    cgltf_float gltfMaxXPoint[3];
    cgltf_float gltfMaxYPoint[3];
    cgltf_float gltfMaxZPoint[3];

    TransformGltfPoint(
        gltfWorld,
        0.0f,
        0.0f,
        0.0f,
        gltfOrigin);

    // A Max-local X unit vector corresponds to glTF-local +X.
    TransformGltfPoint(
        gltfWorld,
        1.0f,
        0.0f,
        0.0f,
        gltfMaxXPoint);

    // A Max-local Y unit vector corresponds to glTF-local -Z.
    TransformGltfPoint(
        gltfWorld,
        0.0f,
        0.0f,
        -1.0f,
        gltfMaxYPoint);

    // A Max-local Z unit vector corresponds to glTF-local +Y.
    TransformGltfPoint(
        gltfWorld,
        0.0f,
        1.0f,
        0.0f,
        gltfMaxZPoint);

    Point3 maxOrigin =
        ConvertGltfLocalPositionToMax(gltfOrigin);

    Point3 maxXPoint =
        ConvertGltfLocalPositionToMax(gltfMaxXPoint);

    Point3 maxYPoint =
        ConvertGltfLocalPositionToMax(gltfMaxYPoint);

    Point3 maxZPoint =
        ConvertGltfLocalPositionToMax(gltfMaxZPoint);

    maxTransform.SetRow(
        0,
        maxXPoint - maxOrigin);

    maxTransform.SetRow(
        1,
        maxYPoint - maxOrigin);

    maxTransform.SetRow(
        2,
        maxZPoint - maxOrigin);

    maxTransform.SetRow(
        3,
        maxOrigin);

    return maxTransform;
}


struct MaxGLBPositionKey
{
    float x;
    float y;
    float z;

    explicit MaxGLBPositionKey(const Point3& point)
        : x(point.x),
          y(point.y),
          z(point.z)
    {
    }

    bool operator<(const MaxGLBPositionKey& other) const
    {
        if (x < other.x)
        {
            return true;
        }

        if (x > other.x)
        {
            return false;
        }

        if (y < other.y)
        {
            return true;
        }

        if (y > other.y)
        {
            return false;
        }

        return z < other.z;
    }
};


static Point3 TransformGltfNormalToMaxWorld(
    const cgltf_float* matrix,
    const cgltf_float* normal)
{
    // glTF matrices are column-major. Build the linear 3x3 part in
    // conventional row/column notation so that we can apply the
    // inverse-transpose required for correct normal transformation.
    const float a00 = static_cast<float>(matrix[0]);
    const float a01 = static_cast<float>(matrix[4]);
    const float a02 = static_cast<float>(matrix[8]);

    const float a10 = static_cast<float>(matrix[1]);
    const float a11 = static_cast<float>(matrix[5]);
    const float a12 = static_cast<float>(matrix[9]);

    const float a20 = static_cast<float>(matrix[2]);
    const float a21 = static_cast<float>(matrix[6]);
    const float a22 = static_cast<float>(matrix[10]);

    const float c00 = a11 * a22 - a12 * a21;
    const float c01 = a12 * a20 - a10 * a22;
    const float c02 = a10 * a21 - a11 * a20;

    const float c10 = a02 * a21 - a01 * a22;
    const float c11 = a00 * a22 - a02 * a20;
    const float c12 = a01 * a20 - a00 * a21;

    const float c20 = a01 * a12 - a02 * a11;
    const float c21 = a02 * a10 - a00 * a12;
    const float c22 = a00 * a11 - a01 * a10;

    const float determinant =
        a00 * c00 +
        a01 * c01 +
        a02 * c02;

    cgltf_float transformedNormal[3];

    if (fabsf(determinant) > 1.0e-20f)
    {
        const float inverseDeterminant =
            1.0f / determinant;

        transformedNormal[0] =
            (c00 * normal[0] +
             c01 * normal[1] +
             c02 * normal[2]) *
            inverseDeterminant;

        transformedNormal[1] =
            (c10 * normal[0] +
             c11 * normal[1] +
             c12 * normal[2]) *
            inverseDeterminant;

        transformedNormal[2] =
            (c20 * normal[0] +
             c21 * normal[1] +
             c22 * normal[2]) *
            inverseDeterminant;
    }
    else
    {
        // Degenerate transforms cannot be inverted. Falling back to the
        // linear transform is better than discarding the imported normal.
        transformedNormal[0] =
            a00 * normal[0] +
            a01 * normal[1] +
            a02 * normal[2];

        transformedNormal[1] =
            a10 * normal[0] +
            a11 * normal[1] +
            a12 * normal[2];

        transformedNormal[2] =
            a20 * normal[0] +
            a21 * normal[1] +
            a22 * normal[2];
    }

    return ConvertGltfLocalNormalToMax(
        transformedNormal);
}



static int GetStableImportImageIndex(
    const cgltf_image* image)
{
    if (image != NULL &&
        g_activeImportData != NULL)
    {
        for (cgltf_size imageIndex = 0;
             imageIndex < g_activeImportData->images_count;
             ++imageIndex)
        {
            if (&g_activeImportData->images[imageIndex] == image)
            {
                if (imageIndex <=
                    static_cast<cgltf_size>(INT_MAX))
                {
                    return static_cast<int>(imageIndex);
                }

                break;
            }
        }
    }

    return g_importTextureSetIndex;
}


static BOOL ImportTextureFileExists(
    const TCHAR* filePath)
{
    if (filePath == NULL ||
        filePath[0] == _T('\0'))
    {
        return FALSE;
    }

    const DWORD attributes =
        GetFileAttributes(filePath);

    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}


static BOOL ExistingFileMatchesBytes(
    const TCHAR* filePath,
    const unsigned char* bytes,
    size_t byteCount)
{
    if (!ImportTextureFileExists(filePath) ||
        bytes == NULL)
    {
        return FALSE;
    }

    FILE* inputFile = NULL;

    if (_tfopen_s(
            &inputFile,
            filePath,
            _T("rb")) != 0 ||
        inputFile == NULL)
    {
        return FALSE;
    }

    if (_fseeki64(inputFile, 0, SEEK_END) != 0)
    {
        fclose(inputFile);
        return FALSE;
    }

    const __int64 fileSize =
        _ftelli64(inputFile);

    if (fileSize < 0 ||
        static_cast<unsigned __int64>(fileSize) !=
            static_cast<unsigned __int64>(byteCount) ||
        _fseeki64(inputFile, 0, SEEK_SET) != 0)
    {
        fclose(inputFile);
        return FALSE;
    }

    unsigned char comparisonBuffer[65536];
    size_t comparedBytes = 0;
    BOOL matches = TRUE;

    while (comparedBytes < byteCount)
    {
        const size_t remaining =
            byteCount - comparedBytes;

        const size_t chunkSize =
            remaining < sizeof(comparisonBuffer)
            ? remaining
            : sizeof(comparisonBuffer);

        const size_t bytesRead =
            fread(
                comparisonBuffer,
                1,
                chunkSize,
                inputFile);

        if (bytesRead != chunkSize ||
            memcmp(
                comparisonBuffer,
                bytes + comparedBytes,
                chunkSize) != 0)
        {
            matches = FALSE;
            break;
        }

        comparedBytes += chunkSize;
    }

    fclose(inputFile);
    return matches;
}


static BOOL BuildTexturePath(
    const TCHAR* sourceFilename,
    const TCHAR* roleName,
    int imageIndex,
    const TCHAR* extension,
    TCHAR* outputPath,
    size_t outputPathCount,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (sourceFilename == NULL ||
        roleName == NULL ||
        extension == NULL ||
        outputPath == NULL ||
        outputPathCount == 0)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Invalid path data while extracting a GLB texture."));
        return FALSE;
    }

    TCHAR sourcePath[MAX_PATH];
    _tcscpy_s(
        sourcePath,
        _countof(sourcePath),
        sourceFilename);

    TCHAR* lastBackslash = _tcsrchr(sourcePath, _T('\\'));
    TCHAR* lastForwardSlash = _tcsrchr(sourcePath, _T('/'));
    TCHAR* lastSlash = lastBackslash;

    if (lastForwardSlash != NULL &&
        (lastSlash == NULL || lastForwardSlash > lastSlash))
    {
        lastSlash = lastForwardSlash;
    }

    TCHAR directory[MAX_PATH];

    if (lastSlash != NULL)
    {
        const size_t directoryLength =
            static_cast<size_t>(lastSlash - sourcePath);

        if (directoryLength >= _countof(directory))
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("The GLB directory path is too long."));
            return FALSE;
        }

        _tcsncpy_s(
            directory,
            _countof(directory),
            sourcePath,
            directoryLength);

        directory[directoryLength] = _T('\0');
    }
    else
    {
        if (GetCurrentDirectory(
                static_cast<DWORD>(_countof(directory)),
                directory) == 0)
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("The current directory could not be determined."));
            return FALSE;
        }
    }

    const TCHAR* fileName =
        lastSlash != NULL
        ? lastSlash + 1
        : sourcePath;

    TCHAR stem[MAX_PATH];
    _tcscpy_s(
        stem,
        _countof(stem),
        fileName);

    TCHAR* lastDot = _tcsrchr(stem, _T('.'));

    if (lastDot != NULL)
    {
        *lastDot = _T('\0');
    }

    if (stem[0] == _T('\0'))
    {
        _tcscpy_s(
            stem,
            _countof(stem),
            _T("GLB"));
    }

    TCHAR textureDirectory[MAX_PATH];

    if (_stprintf_s(
            textureDirectory,
            _countof(textureDirectory),
            _T("%s\\%s_MaxGLB_Textures"),
            directory,
            stem) < 0)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The texture output directory path is too long."));
        return FALSE;
    }

    if (!CreateDirectory(textureDirectory, NULL))
    {
        const DWORD createError = GetLastError();

        if (createError != ERROR_ALREADY_EXISTS)
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("The texture output directory could not be created."));
            return FALSE;
        }
    }

    if (_stprintf_s(
            outputPath,
            outputPathCount,
            _T("%s\\MaxGLB_%s_%d%s"),
            textureDirectory,
            roleName,
            imageIndex,
            extension) < 0)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The extracted texture path is too long."));
        return FALSE;
    }

    return TRUE;
}


static const TCHAR* GetEmbeddedImageExtension(
    const cgltf_image* image)
{
    if (image == NULL || image->mime_type == NULL)
    {
        return NULL;
    }

    if (_stricmp(image->mime_type, "image/png") == 0)
    {
        return _T(".png");
    }

    if (_stricmp(image->mime_type, "image/jpeg") == 0)
    {
        return _T(".jpg");
    }

    return NULL;
}


static BOOL GetEmbeddedImageBytes(
    const cgltf_image* image,
    const unsigned char** outBytes,
    size_t* outByteCount,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (outBytes != NULL)
    {
        *outBytes = NULL;
    }

    if (outByteCount != NULL)
    {
        *outByteCount = 0;
    }

    if (image == NULL ||
        image->buffer_view == NULL ||
        image->buffer_view->buffer == NULL ||
        image->buffer_view->buffer->data == NULL)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("A material image is not embedded in a supported GLB buffer view."));
        return FALSE;
    }

    const cgltf_buffer_view* view =
        image->buffer_view;

    const cgltf_buffer* buffer =
        view->buffer;

    if (view->offset > buffer->size ||
        view->size > buffer->size - view->offset)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("An embedded material image points outside its GLB buffer."));
        return FALSE;
    }

    const unsigned char* bufferBytes =
        static_cast<const unsigned char*>(buffer->data);

    if (outBytes != NULL)
    {
        *outBytes = bufferBytes + view->offset;
    }

    if (outByteCount != NULL)
    {
        *outByteCount =
            static_cast<size_t>(view->size);
    }

    return TRUE;
}


static BOOL ExtractEmbeddedImage(
    const cgltf_image* image,
    const TCHAR* sourceFilename,
    const TCHAR* roleName,
    TCHAR* outputPath,
    size_t outputPathCount,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    const unsigned char* imageBytes = NULL;
    size_t imageByteCount = 0;

    if (!GetEmbeddedImageBytes(
            image,
            &imageBytes,
            &imageByteCount,
            errorMessage,
            errorMessageCount))
    {
        return FALSE;
    }

    const TCHAR* extension =
        GetEmbeddedImageExtension(image);

    if (extension == NULL)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Only embedded PNG and JPEG material images are supported."));
        return FALSE;
    }

    const int imageIndex =
        GetStableImportImageIndex(image);

    if (!BuildTexturePath(
            sourceFilename,
            roleName,
            imageIndex,
            extension,
            outputPath,
            outputPathCount,
            errorMessage,
            errorMessageCount))
    {
        return FALSE;
    }

    if (ExistingFileMatchesBytes(
            outputPath,
            imageBytes,
            imageByteCount))
    {
        g_unchangedImagesThisImport[image] = TRUE;
        return TRUE;
    }

    g_unchangedImagesThisImport[image] = FALSE;

    FILE* outputFile = NULL;

    if (_tfopen_s(
            &outputFile,
            outputPath,
            _T("wb")) != 0 ||
        outputFile == NULL)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("An extracted material image file could not be created."));
        return FALSE;
    }

    const size_t bytesWritten =
        fwrite(
            imageBytes,
            1,
            imageByteCount,
            outputFile);

    fclose(outputFile);

    if (bytesWritten != imageByteCount)
    {
        DeleteFile(outputPath);

        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("An embedded material image could not be written completely."));
        return FALSE;
    }

    return TRUE;
}


static void ReleaseComObject(IUnknown*& object)
{
    if (object != NULL)
    {
        object->Release();
        object = NULL;
    }
}


static BOOL WriteGrayPngWithWic(
    IWICImagingFactory* factory,
    const TCHAR* outputPath,
    UINT width,
    UINT height,
    const std::vector<unsigned char>& pixels,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (factory == NULL ||
        outputPath == NULL ||
        width == 0 ||
        height == 0 ||
        pixels.empty())
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Invalid data while writing a separated ORM channel."));
        return FALSE;
    }

    DeleteFile(outputPath);

    IWICStream* outputStream = NULL;
    IWICBitmapEncoder* encoder = NULL;
    IWICBitmapFrameEncode* frame = NULL;
    IPropertyBag2* propertyBag = NULL;

    HRESULT result =
        factory->CreateStream(&outputStream);

    if (SUCCEEDED(result))
    {
        result =
            outputStream->InitializeFromFilename(
                outputPath,
                GENERIC_WRITE);
    }

    if (SUCCEEDED(result))
    {
        result =
            factory->CreateEncoder(
                GUID_ContainerFormatPng,
                NULL,
                &encoder);
    }

    if (SUCCEEDED(result))
    {
        result =
            encoder->Initialize(
                outputStream,
                WICBitmapEncoderNoCache);
    }

    if (SUCCEEDED(result))
    {
        result =
            encoder->CreateNewFrame(
                &frame,
                &propertyBag);
    }

    if (SUCCEEDED(result))
    {
        result =
            frame->Initialize(propertyBag);
    }

    if (SUCCEEDED(result))
    {
        result =
            frame->SetSize(
                width,
                height);
    }

    WICPixelFormatGUID pixelFormat =
        GUID_WICPixelFormat8bppGray;

    if (SUCCEEDED(result))
    {
        result =
            frame->SetPixelFormat(
                &pixelFormat);
    }

    if (SUCCEEDED(result) &&
        !IsEqualGUID(
            pixelFormat,
            GUID_WICPixelFormat8bppGray))
    {
        result = E_FAIL;
    }

    const UINT stride = width;
    const UINT bufferSize =
        static_cast<UINT>(pixels.size());

    if (SUCCEEDED(result))
    {
        result =
            frame->WritePixels(
                height,
                stride,
                bufferSize,
                const_cast<BYTE*>(
                    reinterpret_cast<const BYTE*>(&pixels[0])));
    }

    if (SUCCEEDED(result))
    {
        result = frame->Commit();
    }

    if (SUCCEEDED(result))
    {
        result = encoder->Commit();
    }

    if (propertyBag != NULL)
    {
        propertyBag->Release();
        propertyBag = NULL;
    }

    if (frame != NULL)
    {
        frame->Release();
        frame = NULL;
    }

    if (encoder != NULL)
    {
        encoder->Release();
        encoder = NULL;
    }

    if (outputStream != NULL)
    {
        outputStream->Release();
        outputStream = NULL;
    }

    if (FAILED(result))
    {
        DeleteFile(outputPath);

        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("A separated ORM channel could not be written as PNG."));
        return FALSE;
    }

    return TRUE;
}


static BOOL ExtractBaseColorAlphaTexture(
    const cgltf_image* image,
    const TCHAR* sourceFilename,
    TCHAR* outputPath,
    size_t outputPathCount,
    BOOL* outHasTransparency,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (outHasTransparency != NULL)
    {
        *outHasTransparency =
            FALSE;
    }

    const unsigned char* imageBytes = NULL;
    size_t imageByteCount = 0;

    if (!GetEmbeddedImageBytes(
            image,
            &imageBytes,
            &imageByteCount,
            errorMessage,
            errorMessageCount))
    {
        return FALSE;
    }

    if (imageByteCount == 0 ||
        imageByteCount >
            static_cast<size_t>(UINT_MAX))
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The base-color image is empty or too large for alpha extraction."));
        return FALSE;
    }

    const int imageIndex =
        GetStableImportImageIndex(
            image);

    if (!BuildTexturePath(
            sourceFilename,
            _T("Opacity"),
            imageIndex,
            _T(".png"),
            outputPath,
            outputPathCount,
            errorMessage,
            errorMessageCount))
    {
        return FALSE;
    }

    HRESULT comResult =
        CoInitializeEx(
            NULL,
            COINIT_APARTMENTTHREADED);

    const BOOL shouldUninitialize =
        SUCCEEDED(comResult);

    if (FAILED(comResult) &&
        comResult != RPC_E_CHANGED_MODE)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("COM could not be initialized for alpha extraction."));
        return FALSE;
    }

    IWICImagingFactory* factory = NULL;
    IWICStream* inputStream = NULL;
    IWICBitmapDecoder* decoder = NULL;
    IWICBitmapFrameDecode* sourceFrame = NULL;
    IWICFormatConverter* converter = NULL;

    HRESULT result =
        CoCreateInstance(
            CLSID_WICImagingFactory,
            NULL,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));

    if (SUCCEEDED(result))
    {
        result =
            factory->CreateStream(
                &inputStream);
    }

    if (SUCCEEDED(result))
    {
        result =
            inputStream->InitializeFromMemory(
                const_cast<BYTE*>(
                    reinterpret_cast<const BYTE*>(
                        imageBytes)),
                static_cast<DWORD>(
                    imageByteCount));
    }

    if (SUCCEEDED(result))
    {
        result =
            factory->CreateDecoderFromStream(
                inputStream,
                NULL,
                WICDecodeMetadataCacheOnLoad,
                &decoder);
    }

    if (SUCCEEDED(result))
    {
        result =
            decoder->GetFrame(
                0,
                &sourceFrame);
    }

    if (SUCCEEDED(result))
    {
        result =
            factory->CreateFormatConverter(
                &converter);
    }

    if (SUCCEEDED(result))
    {
        result =
            converter->Initialize(
                sourceFrame,
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                NULL,
                0.0,
                WICBitmapPaletteTypeCustom);
    }

    UINT width = 0;
    UINT height = 0;

    if (SUCCEEDED(result))
    {
        result =
            converter->GetSize(
                &width,
                &height);
    }

    if (SUCCEEDED(result) &&
        (width == 0 ||
         height == 0 ||
         width > UINT_MAX / 4 ||
         height > UINT_MAX / (width * 4)))
    {
        result = E_FAIL;
    }

    std::vector<unsigned char> rgbaPixels;
    std::vector<unsigned char> alphaPixels;
    BOOL hasTransparency = FALSE;

    if (SUCCEEDED(result))
    {
        const size_t pixelCount =
            static_cast<size_t>(width) *
            static_cast<size_t>(height);

        rgbaPixels.resize(
            pixelCount * 4);

        alphaPixels.resize(
            pixelCount);

        result =
            converter->CopyPixels(
                NULL,
                width * 4,
                static_cast<UINT>(
                    rgbaPixels.size()),
                reinterpret_cast<BYTE*>(
                    &rgbaPixels[0]));

        if (SUCCEEDED(result))
        {
            for (size_t pixelIndex = 0;
                 pixelIndex < pixelCount;
                 ++pixelIndex)
            {
                const unsigned char alpha =
                    rgbaPixels[
                        pixelIndex * 4 + 3];

                alphaPixels[pixelIndex] =
                    alpha;

                if (alpha < 255)
                {
                    hasTransparency =
                        TRUE;
                }
            }
        }
    }

    if (converter != NULL)
    {
        converter->Release();
    }

    if (sourceFrame != NULL)
    {
        sourceFrame->Release();
    }

    if (decoder != NULL)
    {
        decoder->Release();
    }

    if (inputStream != NULL)
    {
        inputStream->Release();
    }

    if (SUCCEEDED(result) &&
        hasTransparency)
    {
        if (!WriteGrayPngWithWic(
                factory,
                outputPath,
                width,
                height,
                alphaPixels,
                errorMessage,
                errorMessageCount))
        {
            result = E_FAIL;
        }
    }
    else if (SUCCEEDED(result))
    {
        DeleteFile(
            outputPath);
    }

    if (factory != NULL)
    {
        factory->Release();
    }

    if (shouldUninitialize)
    {
        CoUninitialize();
    }

    if (FAILED(result))
    {
        if (errorMessage[0] ==
            _T('\0'))
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("The base-color alpha channel could not be decoded."));
        }

        return FALSE;
    }

    if (outHasTransparency != NULL)
    {
        *outHasTransparency =
            hasTransparency;
    }

    return TRUE;
}


static BOOL SplitOrmTexture(
    const cgltf_image* image,
    const TCHAR* sourceFilename,
    TCHAR* outOcclusionPath,
    size_t outOcclusionPathCount,
    TCHAR* outGlossinessPath,
    size_t outGlossinessPathCount,
    TCHAR* outMetallicPath,
    size_t outMetallicPathCount,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    const unsigned char* imageBytes = NULL;
    size_t imageByteCount = 0;

    if (!GetEmbeddedImageBytes(
            image,
            &imageBytes,
            &imageByteCount,
            errorMessage,
            errorMessageCount))
    {
        return FALSE;
    }

    if (imageByteCount > static_cast<size_t>(UINT_MAX))
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The packed ORM image is too large for Windows Imaging Component."));
        return FALSE;
    }

    const int imageIndex =
        GetStableImportImageIndex(image);

    if (!BuildTexturePath(
            sourceFilename,
            _T("Occlusion"),
            imageIndex,
            _T(".png"),
            outOcclusionPath,
            outOcclusionPathCount,
            errorMessage,
            errorMessageCount) ||
        !BuildTexturePath(
            sourceFilename,
            _T("Glossiness"),
            imageIndex,
            _T(".png"),
            outGlossinessPath,
            outGlossinessPathCount,
            errorMessage,
            errorMessageCount) ||
        !BuildTexturePath(
            sourceFilename,
            _T("Metallic"),
            imageIndex,
            _T(".png"),
            outMetallicPath,
            outMetallicPathCount,
            errorMessage,
            errorMessageCount))
    {
        return FALSE;
    }

    const BOOL alreadySplitThisImport =
        g_splitOrmImagesThisImport.find(image) !=
            g_splitOrmImagesThisImport.end();

    const std::map<const cgltf_image*, BOOL>::const_iterator unchangedIterator =
        g_unchangedImagesThisImport.find(image);

    const BOOL sourceWasUnchanged =
        unchangedIterator != g_unchangedImagesThisImport.end() &&
        unchangedIterator->second;

    if ((alreadySplitThisImport || sourceWasUnchanged) &&
        ImportTextureFileExists(outOcclusionPath) &&
        ImportTextureFileExists(outGlossinessPath) &&
        ImportTextureFileExists(outMetallicPath))
    {
        g_splitOrmImagesThisImport[image] = TRUE;
        return TRUE;
    }

    HRESULT comResult =
        CoInitializeEx(
            NULL,
            COINIT_APARTMENTTHREADED);

    const BOOL shouldUninitialize =
        SUCCEEDED(comResult);

    if (FAILED(comResult) &&
        comResult != RPC_E_CHANGED_MODE)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("COM could not be initialized for ORM channel extraction."));
        return FALSE;
    }

    IWICImagingFactory* factory = NULL;
    IWICStream* inputStream = NULL;
    IWICBitmapDecoder* decoder = NULL;
    IWICBitmapFrameDecode* sourceFrame = NULL;
    IWICFormatConverter* converter = NULL;

    HRESULT result =
        CoCreateInstance(
            CLSID_WICImagingFactory,
            NULL,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));

    if (SUCCEEDED(result))
    {
        result =
            factory->CreateStream(
                &inputStream);
    }

    if (SUCCEEDED(result))
    {
        result =
            inputStream->InitializeFromMemory(
                const_cast<BYTE*>(
                    reinterpret_cast<const BYTE*>(imageBytes)),
                static_cast<DWORD>(imageByteCount));
    }

    if (SUCCEEDED(result))
    {
        result =
            factory->CreateDecoderFromStream(
                inputStream,
                NULL,
                WICDecodeMetadataCacheOnLoad,
                &decoder);
    }

    if (SUCCEEDED(result))
    {
        result =
            decoder->GetFrame(
                0,
                &sourceFrame);
    }

    if (SUCCEEDED(result))
    {
        result =
            factory->CreateFormatConverter(
                &converter);
    }

    if (SUCCEEDED(result))
    {
        result =
            converter->Initialize(
                sourceFrame,
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                NULL,
                0.0,
                WICBitmapPaletteTypeCustom);
    }

    UINT width = 0;
    UINT height = 0;

    if (SUCCEEDED(result))
    {
        result =
            converter->GetSize(
                &width,
                &height);
    }

    if (SUCCEEDED(result) &&
        (width == 0 ||
         height == 0 ||
         width > UINT_MAX / 4 ||
         height > UINT_MAX / (width * 4)))
    {
        result = E_FAIL;
    }

    std::vector<unsigned char> rgbaPixels;
    std::vector<unsigned char> occlusionPixels;
    std::vector<unsigned char> glossinessPixels;
    std::vector<unsigned char> metallicPixels;

    if (SUCCEEDED(result))
    {
        const size_t pixelCount =
            static_cast<size_t>(width) *
            static_cast<size_t>(height);

        rgbaPixels.resize(pixelCount * 4);
        occlusionPixels.resize(pixelCount);
        glossinessPixels.resize(pixelCount);
        metallicPixels.resize(pixelCount);

        const UINT rgbaStride =
            width * 4;

        result =
            converter->CopyPixels(
                NULL,
                rgbaStride,
                static_cast<UINT>(rgbaPixels.size()),
                reinterpret_cast<BYTE*>(&rgbaPixels[0]));

        if (SUCCEEDED(result))
        {
            for (size_t pixelIndex = 0;
                 pixelIndex < pixelCount;
                 ++pixelIndex)
            {
                const size_t rgbaIndex =
                    pixelIndex * 4;

                const unsigned char occlusion =
                    rgbaPixels[rgbaIndex + 0];

                const unsigned char roughness =
                    rgbaPixels[rgbaIndex + 1];

                const unsigned char metallic =
                    rgbaPixels[rgbaIndex + 2];

                occlusionPixels[pixelIndex] =
                    occlusion;

                // glTF stores roughness. The old Standard material expects
                // glossiness, so the green channel must be inverted.
                glossinessPixels[pixelIndex] =
                    static_cast<unsigned char>(
                        255 - roughness);

                metallicPixels[pixelIndex] =
                    metallic;
            }
        }
    }

    if (converter != NULL)
    {
        converter->Release();
        converter = NULL;
    }

    if (sourceFrame != NULL)
    {
        sourceFrame->Release();
        sourceFrame = NULL;
    }

    if (decoder != NULL)
    {
        decoder->Release();
        decoder = NULL;
    }

    if (inputStream != NULL)
    {
        inputStream->Release();
        inputStream = NULL;
    }

    if (SUCCEEDED(result))
    {
        if (!WriteGrayPngWithWic(
                factory,
                outOcclusionPath,
                width,
                height,
                occlusionPixels,
                errorMessage,
                errorMessageCount) ||
            !WriteGrayPngWithWic(
                factory,
                outGlossinessPath,
                width,
                height,
                glossinessPixels,
                errorMessage,
                errorMessageCount) ||
            !WriteGrayPngWithWic(
                factory,
                outMetallicPath,
                width,
                height,
                metallicPixels,
                errorMessage,
                errorMessageCount))
        {
            result = E_FAIL;
        }
    }

    if (factory != NULL)
    {
        factory->Release();
        factory = NULL;
    }

    if (shouldUninitialize)
    {
        CoUninitialize();
    }

    if (FAILED(result))
    {
        if (errorMessage[0] == _T('\0'))
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("The packed ORM texture could not be decoded."));
        }

        return FALSE;
    }

    g_splitOrmImagesThisImport[image] = TRUE;
    return TRUE;
}


static void AppendMaterialImportLog(
    const TCHAR* message)
{
    TCHAR tempDirectory[MAX_PATH];

    const DWORD tempLength = GetTempPath(
        static_cast<DWORD>(_countof(tempDirectory)),
        tempDirectory);

    if (tempLength == 0 ||
        tempLength >= _countof(tempDirectory))
    {
        return;
    }

    TCHAR logPath[MAX_PATH];

    if (_stprintf_s(
            logPath,
            _countof(logPath),
            _T("%sMaxGLB2016_material.log"),
            tempDirectory) < 0)
    {
        return;
    }

    FILE* logFile = NULL;

    if (_tfopen_s(
            &logFile,
            logPath,
            _T("a, ccs=UTF-8")) != 0 ||
        logFile == NULL)
    {
        return;
    }

    _ftprintf(
        logFile,
        _T("%s\n"),
        message != NULL ? message : _T("(null)"));

    fclose(logFile);
}


static BitmapTex* CreateBitmapTexture(
    const TCHAR* textureName,
    const TCHAR* filePath)
{
    BitmapTex* bitmapTexture =
        NewDefaultBitmapTex();

    if (bitmapTexture == NULL)
    {
        return NULL;
    }

    bitmapTexture->SetName(textureName);
    bitmapTexture->SetMapName(filePath);
    bitmapTexture->NotifyDependents(
        FOREVER,
        PART_ALL,
        REFMSG_CHANGE);

    return bitmapTexture;
}


static BOOL AssignStdMaterialMap(
    StdMat2* material,
    int standardMapId,
    Texmap* texture,
    float amount,
    TimeValue timeValue)
{
    if (material == NULL ||
        texture == NULL)
    {
        return FALSE;
    }

    const int slot =
        material->StdIDToChannel(
            standardMapId);

    if (slot < 0)
    {
        return FALSE;
    }

    material->SetSubTexmap(
        slot,
        texture);

    material->SetTexmapAmt(
        standardMapId,
        amount,
        timeValue);

    material->EnableMap(
        standardMapId,
        TRUE);

    return TRUE;
}


static BOOL CreateStandardMaterialForPrimitive(
    const cgltf_primitive* primitive,
    const TCHAR* sourceFilename,
    Interface* maxInterface,
    Mtl** outMaterial,
    BOOL* outHasBaseColorTexture,
    BOOL* outHasNormalTexture,
    BOOL* outHasOrmChannels,
    BOOL* outHasEmissiveTexture,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (outMaterial != NULL)
    {
        *outMaterial = NULL;
    }

    if (outHasBaseColorTexture != NULL)
    {
        *outHasBaseColorTexture = FALSE;
    }

    if (outHasNormalTexture != NULL)
    {
        *outHasNormalTexture = FALSE;
    }

    if (outHasOrmChannels != NULL)
    {
        *outHasOrmChannels = FALSE;
    }

    if (outHasEmissiveTexture != NULL)
    {
        *outHasEmissiveTexture = FALSE;
    }

    if (primitive == NULL ||
        primitive->material == NULL)
    {
        return TRUE;
    }

    AppendMaterialImportLog(_T("01: material creation started"));

    const cgltf_material* gltfMaterial =
        primitive->material;

    StdMat2* maxMaterial =
        NewDefaultStdMat();

    if (maxMaterial == NULL)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("3ds Max could not create a Standard material."));
        return FALSE;
    }

    AppendMaterialImportLog(_T("02: Standard material created"));

    if (gltfMaterial->name != NULL &&
        gltfMaterial->name[0] != '\0')
    {
        TCHAR materialName[256];

#if defined(UNICODE) || defined(_UNICODE)
        if (MultiByteToWideChar(
                CP_UTF8,
                0,
                gltfMaterial->name,
                -1,
                materialName,
                static_cast<int>(_countof(materialName))) == 0)
        {
            _tcscpy_s(
                materialName,
                _countof(materialName),
                _T("MaxGLB2016_Material_0"));
        }
#else
        _tcscpy_s(
            materialName,
            _countof(materialName),
            gltfMaterial->name);
#endif

        maxMaterial->SetName(materialName);
    }
    else
    {
        maxMaterial->SetName(
            _T("MaxGLB2016_Material_0"));
    }

    const TimeValue timeValue =
        maxInterface != NULL
        ? maxInterface->GetTime()
        : 0;

    Color baseColor(1.0f, 1.0f, 1.0f);
    float opacity = 1.0f;

    const int importedAlphaMode =
        ConvertCgltfAlphaMode(
            gltfMaterial->alpha_mode);

    const float importedAlphaCutoff =
        importedAlphaMode ==
            MAXGLB_ALPHA_MASK
        ? ClampMaxGLBAlphaCutoff(
            static_cast<float>(
                gltfMaterial->alpha_cutoff))
        : 0.5f;

    if (gltfMaterial->has_pbr_metallic_roughness)
    {
        const cgltf_pbr_metallic_roughness& pbr =
            gltfMaterial->pbr_metallic_roughness;

        baseColor = Color(
            static_cast<float>(pbr.base_color_factor[0]),
            static_cast<float>(pbr.base_color_factor[1]),
            static_cast<float>(pbr.base_color_factor[2]));

        opacity =
            static_cast<float>(pbr.base_color_factor[3]);
    }

    maxMaterial->SetDiffuse(
        baseColor,
        timeValue);

    maxMaterial->SetAmbient(
        baseColor,
        timeValue);

    maxMaterial->SetSpecular(
        Color(1.0f, 1.0f, 1.0f),
        timeValue);

    // OPAQUE ignores both base-color factor alpha and image alpha.
    maxMaterial->SetOpacity(
        importedAlphaMode ==
            MAXGLB_ALPHA_OPAQUE
        ? 1.0f
        : opacity,
        timeValue);

    BOOL hasBaseColorTexture = FALSE;
    BOOL hasNormalTexture = FALSE;
    BOOL hasOrmChannels = FALSE;
    BOOL hasEmissiveTexture = FALSE;

    if (g_activeImportSettings.importTextures)
    {
    if (gltfMaterial->has_pbr_metallic_roughness)
    {
        const cgltf_texture_view& baseColorView =
            gltfMaterial->pbr_metallic_roughness.base_color_texture;

        if (baseColorView.texture != NULL &&
            baseColorView.texture->image != NULL)
        {
            TCHAR baseColorPath[MAX_PATH];

            AppendMaterialImportLog(_T("03: extracting base color"));

            if (!ExtractEmbeddedImage(
                    baseColorView.texture->image,
                    sourceFilename,
                    _T("BaseColor"),
                    baseColorPath,
                    _countof(baseColorPath),
                    errorMessage,
                    errorMessageCount))
            {
                maxMaterial->DeleteThis();
                return FALSE;
            }

            BitmapTex* baseColorBitmap =
                CreateBitmapTexture(
                    _T("MaxGLB2016_BaseColor_0"),
                    baseColorPath);

            if (baseColorBitmap == NULL ||
                !AssignStdMaterialMap(
                    maxMaterial,
                    ID_DI,
                    baseColorBitmap,
                    1.0f,
                    timeValue))
            {
                if (baseColorBitmap != NULL)
                {
                    baseColorBitmap->DeleteThis();
                }

                maxMaterial->DeleteThis();

                _tcscpy_s(
                    errorMessage,
                    errorMessageCount,
                    _T("The base-color texture could not be assigned."));
                return FALSE;
            }

            maxMaterial->SetActiveTexmap(
                baseColorBitmap);

            baseColorBitmap->SetMtlFlag(
                MTL_TEX_DISPLAY_ENABLED,
                TRUE);

            baseColorBitmap->ActivateTexDisplay(TRUE);

            maxMaterial->SetMtlFlag(
                MTL_TEX_DISPLAY_ENABLED,
                TRUE);

            if (maxInterface != NULL)
            {
                maxInterface->ActivateTexture(
                    baseColorBitmap,
                    maxMaterial);
            }

            hasBaseColorTexture = TRUE;
            AppendMaterialImportLog(_T("04: base color assigned"));

            if (importedAlphaMode !=
                    MAXGLB_ALPHA_OPAQUE)
            {
                TCHAR opacityPath[MAX_PATH];
                BOOL hasTextureTransparency =
                    FALSE;

                if (!ExtractBaseColorAlphaTexture(
                        baseColorView.texture->image,
                        sourceFilename,
                        opacityPath,
                        _countof(opacityPath),
                        &hasTextureTransparency,
                        errorMessage,
                        errorMessageCount))
                {
                    maxMaterial->DeleteThis();
                    return FALSE;
                }

                if (hasTextureTransparency)
                {
                    BitmapTex* opacityBitmap =
                        CreateBitmapTexture(
                            _T("MaxGLB2016_Opacity_0"),
                            opacityPath);

                    if (opacityBitmap == NULL ||
                        !AssignStdMaterialMap(
                            maxMaterial,
                            ID_OP,
                            opacityBitmap,
                            1.0f,
                            timeValue))
                    {
                        if (opacityBitmap != NULL)
                        {
                            opacityBitmap->DeleteThis();
                        }

                        maxMaterial->DeleteThis();

                        _tcscpy_s(
                            errorMessage,
                            errorMessageCount,
                            _T("The base-color alpha channel could not be assigned to the opacity slot."));
                        return FALSE;
                    }

                    StoreMaxGLBAlphaMetadata(
                        maxMaterial,
                        importedAlphaMode,
                        importedAlphaCutoff,
                        MAXGLB_ALPHA_FLAG_OPACITY_FROM_BASE_ALPHA);
                }
            }
        }
    }

    if (gltfMaterial->normal_texture.texture != NULL &&
        gltfMaterial->normal_texture.texture->image != NULL)
    {
        TCHAR normalPath[MAX_PATH];

        if (!ExtractEmbeddedImage(
                gltfMaterial->normal_texture.texture->image,
                sourceFilename,
                _T("Normal"),
                normalPath,
                _countof(normalPath),
                errorMessage,
                errorMessageCount))
        {
            maxMaterial->DeleteThis();
            return FALSE;
        }

        BitmapTex* normalBitmap =
            CreateBitmapTexture(
                _T("MaxGLB2016_Normal_0"),
                normalPath);

        if (normalBitmap == NULL)
        {
            maxMaterial->DeleteThis();

            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("The normal texture bitmap could not be created."));
            return FALSE;
        }

        AppendMaterialImportLog(_T("05: normal bitmap created"));

        // Safe Max 2016 path: assign the bitmap directly to the Bump slot.
        // A Generic Normal wrapper will be added only after the legacy
        // material graph is proven stable.
        if (!AssignStdMaterialMap(
                maxMaterial,
                ID_BU,
                normalBitmap,
                1.0f,
                timeValue))
        {
            normalBitmap->DeleteThis();
            maxMaterial->DeleteThis();

            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("The normal texture could not be assigned to the bump slot."));
            return FALSE;
        }

        hasNormalTexture = TRUE;
        AppendMaterialImportLog(_T("06: normal bitmap assigned directly to Bump"));
    }

    const cgltf_texture_view* ormTextureView = NULL;

    if (gltfMaterial->has_pbr_metallic_roughness &&
        gltfMaterial->pbr_metallic_roughness
            .metallic_roughness_texture.texture != NULL)
    {
        ormTextureView =
            &gltfMaterial->pbr_metallic_roughness
                .metallic_roughness_texture;
    }
    else if (gltfMaterial->occlusion_texture.texture != NULL)
    {
        ormTextureView =
            &gltfMaterial->occlusion_texture;
    }

    if (ormTextureView != NULL &&
        ormTextureView->texture != NULL &&
        ormTextureView->texture->image != NULL)
    {
        const cgltf_image* ormImage =
            ormTextureView->texture->image;

        TCHAR packedOrmPath[MAX_PATH];
        TCHAR occlusionPath[MAX_PATH];
        TCHAR glossinessPath[MAX_PATH];
        TCHAR metallicPath[MAX_PATH];

        AppendMaterialImportLog(_T("07: extracting and splitting ORM"));

        if (!ExtractEmbeddedImage(
                ormImage,
                sourceFilename,
                _T("OcclusionRoughnessMetallic"),
                packedOrmPath,
                _countof(packedOrmPath),
                errorMessage,
                errorMessageCount) ||
            !SplitOrmTexture(
                ormImage,
                sourceFilename,
                occlusionPath,
                _countof(occlusionPath),
                glossinessPath,
                _countof(glossinessPath),
                metallicPath,
                _countof(metallicPath),
                errorMessage,
                errorMessageCount))
        {
            maxMaterial->DeleteThis();
            return FALSE;
        }

        BitmapTex* occlusionBitmap =
            CreateBitmapTexture(
                _T("MaxGLB2016_Occlusion_0"),
                occlusionPath);

        BitmapTex* glossinessBitmap =
            CreateBitmapTexture(
                _T("MaxGLB2016_Glossiness_0"),
                glossinessPath);

        BitmapTex* metallicBitmap =
            CreateBitmapTexture(
                _T("MaxGLB2016_Metallic_0"),
                metallicPath);

        if (occlusionBitmap == NULL ||
            glossinessBitmap == NULL ||
            metallicBitmap == NULL)
        {
            if (occlusionBitmap != NULL)
            {
                occlusionBitmap->DeleteThis();
            }

            if (glossinessBitmap != NULL)
            {
                glossinessBitmap->DeleteThis();
            }

            if (metallicBitmap != NULL)
            {
                metallicBitmap->DeleteThis();
            }

            maxMaterial->DeleteThis();

            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("One or more separated ORM bitmap textures could not be created."));
            return FALSE;
        }

        if (!AssignStdMaterialMap(
                maxMaterial,
                ID_AM,
                occlusionBitmap,
                1.0f,
                timeValue) ||
            !AssignStdMaterialMap(
                maxMaterial,
                ID_SH,
                glossinessBitmap,
                1.0f,
                timeValue) ||
            !AssignStdMaterialMap(
                maxMaterial,
                ID_SS,
                metallicBitmap,
                1.0f,
                timeValue))
        {
            maxMaterial->DeleteThis();

            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("The separated ORM maps could not be assigned to the Standard material."));
            return FALSE;
        }

        hasOrmChannels = TRUE;
        AppendMaterialImportLog(_T("08: ORM maps assigned"));
    }

    if (gltfMaterial->emissive_texture.texture != NULL &&
        gltfMaterial->emissive_texture.texture->image != NULL)
    {
        TCHAR emissivePath[MAX_PATH];

        if (!ExtractEmbeddedImage(
                gltfMaterial->emissive_texture.texture->image,
                sourceFilename,
                _T("Emissive"),
                emissivePath,
                _countof(emissivePath),
                errorMessage,
                errorMessageCount))
        {
            maxMaterial->DeleteThis();
            return FALSE;
        }

        BitmapTex* emissiveBitmap =
            CreateBitmapTexture(
                _T("MaxGLB2016_Emissive_0"),
                emissivePath);

        if (emissiveBitmap == NULL ||
            !AssignStdMaterialMap(
                maxMaterial,
                ID_SI,
                emissiveBitmap,
                1.0f,
                timeValue))
        {
            if (emissiveBitmap != NULL)
            {
                emissiveBitmap->DeleteThis();
            }

            maxMaterial->DeleteThis();

            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("The emissive texture could not be assigned."));
            return FALSE;
        }

        hasEmissiveTexture = TRUE;
        AppendMaterialImportLog(_T("09: emissive assigned"));
    }

    }

    int existingAlphaMode =
        MAXGLB_ALPHA_OPAQUE;

    float existingAlphaCutoff =
        0.5f;

    DWORD existingAlphaFlags = 0;

    ReadMaxGLBAlphaMetadata(
        maxMaterial,
        &existingAlphaMode,
        &existingAlphaCutoff,
        &existingAlphaFlags);

    StoreMaxGLBAlphaMetadata(
        maxMaterial,
        importedAlphaMode,
        importedAlphaCutoff,
        existingAlphaFlags);

    AppendMaterialImportLog(_T("10: material graph complete"));

    maxMaterial->NotifyDependents(
        FOREVER,
        PART_ALL,
        REFMSG_CHANGE);

    AppendMaterialImportLog(_T("11: material dependents notified"));

    if (outMaterial != NULL)
    {
        *outMaterial = maxMaterial;
    }

    if (outHasBaseColorTexture != NULL)
    {
        *outHasBaseColorTexture =
            hasBaseColorTexture;
    }

    if (outHasNormalTexture != NULL)
    {
        *outHasNormalTexture =
            hasNormalTexture;
    }

    if (outHasOrmChannels != NULL)
    {
        *outHasOrmChannels =
            hasOrmChannels;
    }

    if (outHasEmissiveTexture != NULL)
    {
        *outHasEmissiveTexture =
            hasEmissiveTexture;
    }

    return TRUE;
}


struct MaxGLBImportStats
{
    int objectCount;
    int primitiveCount;
    int totalVertexCount;
    int totalFaceCount;
    int objectsWithUv;
    int objectsWithNormals;
    int materials;
    int baseColorTextures;
    int normalTextures;
    int ormTextures;
    int emissiveTextures;
    int nextObjectIndex;
    BOOL createdParentNode;

    std::vector<INode*> createdNodes;

    MaxGLBImportStats()
        : objectCount(0)
        , primitiveCount(0)
        , totalVertexCount(0)
        , totalFaceCount(0)
        , objectsWithUv(0)
        , objectsWithNormals(0)
        , materials(0)
        , baseColorTextures(0)
        , normalTextures(0)
        , ormTextures(0)
        , emissiveTextures(0)
        , nextObjectIndex(0)
        , createdParentNode(FALSE)
    {
    }
};


static void ConvertUtf8ToTchar(
    const char* utf8Text,
    TCHAR* outputText,
    size_t outputTextCount)
{
    if (outputText == NULL ||
        outputTextCount == 0)
    {
        return;
    }

    outputText[0] = _T('\0');

    if (utf8Text == NULL ||
        utf8Text[0] == '\0')
    {
        return;
    }

#if defined(UNICODE) || defined(_UNICODE)
    if (MultiByteToWideChar(
            CP_UTF8,
            0,
            utf8Text,
            -1,
            outputText,
            static_cast<int>(outputTextCount)) == 0)
    {
        outputText[0] = _T('\0');
    }
#else
    strncpy_s(
        outputText,
        outputTextCount,
        utf8Text,
        _TRUNCATE);
#endif
}



#define MAXGLB_ID_IMPORT_TEXTURES      4103
#define MAXGLB_ID_ADD_PARENT           4104
#define MAXGLB_ID_IMPORT_ANIMATION     4105
#define MAXGLB_ID_INFO                 4106
#define MAXGLB_ID_IMPORT               4107
#define MAXGLB_ID_CANCEL               4108
#define MAXGLB_ID_OBJECT_LIST          4109
#define MAXGLB_ID_MATERIAL_LIST        4110
#define MAXGLB_ID_TEXTURE_LIST         4111
#define MAXGLB_ID_NORMALIZE_SIZE       4112
#define MAXGLB_ID_NORMALIZE_TARGET     4113
#define MAXGLB_ID_PREVIEW_ROTATE_UP    4114
#define MAXGLB_ID_PREVIEW_ROTATE_DOWN  4115
#define MAXGLB_ID_PREVIEW_ROTATE_LEFT  4116
#define MAXGLB_ID_PREVIEW_ROTATE_RIGHT 4117
#define MAXGLB_ID_PREVIEW_ROTATE_RESET 4118


#define MAXGLB_ID_EXPORT_SCALE             4201
#define MAXGLB_ID_EXPORT_NORMALIZE         4202
#define MAXGLB_ID_EXPORT_NORMALIZE_TARGET  4203
#define MAXGLB_ID_EXPORT_MATERIALS         4204
#define MAXGLB_ID_EXPORT_TEXTURES          4205
#define MAXGLB_ID_EXPORT_SUMMARY           4206
#define MAXGLB_ID_EXPORT_EXPLORER          4207
#define MAXGLB_ID_EXPORT_ANIMATION         4208
#define MAXGLB_ID_EXPORT_INFO              4209
#define MAXGLB_ID_EXPORT_CANCEL            4210
#define MAXGLB_ID_EXPORT_CONFIRM           4211
#define MAXGLB_ID_EXPORT_ALPHA_MODE        4212
#define MAXGLB_ID_EXPORT_ALPHA_CUTOFF      4213


struct MaxGLBImportDialogContext
{
    HWND parentWindow;
    HWND window;
    HWND previewWindow;
    HWND previewRotateUpButton;
    HWND previewRotateDownButton;
    HWND previewRotateLeftButton;
    HWND previewRotateRightButton;
    HWND previewRotateResetButton;
    HWND importTexturesCheck;
    HWND addParentCheck;
    HWND normalizeSizeCheck;
    HWND normalizeTargetEdit;
    HWND animationCheck;

    const cgltf_data* data;
    const TCHAR* filename;

    MaxGLBImportSettings settings;

    BOOL accepted;
    BOOL closed;
    int importableObjectCount;

    std::vector<Point3> previewPoints;

    MaxGLBImportDialogContext()
        : parentWindow(NULL)
        , window(NULL)
        , previewWindow(NULL)
        , previewRotateUpButton(NULL)
        , previewRotateDownButton(NULL)
        , previewRotateLeftButton(NULL)
        , previewRotateRightButton(NULL)
        , previewRotateResetButton(NULL)
        , importTexturesCheck(NULL)
        , addParentCheck(NULL)
        , normalizeSizeCheck(NULL)
        , normalizeTargetEdit(NULL)
        , animationCheck(NULL)
        , data(NULL)
        , filename(NULL)
        , accepted(FALSE)
        , closed(FALSE)
        , importableObjectCount(0)
    {
    }
};


static const TCHAR* MAXGLB_IMPORT_WINDOW_CLASS =
    _T("MaxGLB2016ImportOptionsWindow");

static const TCHAR* MAXGLB_PREVIEW_WINDOW_CLASS =
    _T("MaxGLB2016PreviewWindow");


static void UseReadableClassicTheme(
    HWND control)
{
    if (control != NULL)
    {
        // 3ds Max 2016 may apply its dark UI theme to controls created by
        // plug-ins, while the dialog itself remains light. Disabling the
        // visual style on form fields gives us predictable high contrast.
        SetWindowTheme(
            control,
            L"",
            L"");
    }
}


static BOOL GetImportCheckboxState(
    HWND control)
{
    return control != NULL &&
        GetWindowLongPtr(
            control,
            GWLP_USERDATA) != 0;
}


static void SetImportCheckboxState(
    HWND control,
    BOOL checked)
{
    if (control == NULL)
    {
        return;
    }

    SetWindowLongPtr(
        control,
        GWLP_USERDATA,
        checked ? 1 : 0);

    InvalidateRect(
        control,
        NULL,
        TRUE);
}


static BOOL IsImportCheckboxId(
    int controlId)
{
    return controlId == MAXGLB_ID_IMPORT_TEXTURES ||
        controlId == MAXGLB_ID_ADD_PARENT ||
        controlId == MAXGLB_ID_NORMALIZE_SIZE ||
        controlId == MAXGLB_ID_IMPORT_ANIMATION;
}


static BOOL IsImportPushButtonId(
    int controlId)
{
    return controlId == MAXGLB_ID_INFO ||
        controlId == MAXGLB_ID_CANCEL ||
        controlId == MAXGLB_ID_IMPORT ||
        controlId == MAXGLB_ID_PREVIEW_ROTATE_UP ||
        controlId == MAXGLB_ID_PREVIEW_ROTATE_DOWN ||
        controlId == MAXGLB_ID_PREVIEW_ROTATE_LEFT ||
        controlId == MAXGLB_ID_PREVIEW_ROTATE_RIGHT ||
        controlId == MAXGLB_ID_PREVIEW_ROTATE_RESET;
}


static void DrawLightImportCheckbox(
    const DRAWITEMSTRUCT* drawItem)
{
    if (drawItem == NULL)
    {
        return;
    }

    HDC deviceContext =
        drawItem->hDC;

    RECT itemRect =
        drawItem->rcItem;

    const BOOL disabled =
        (drawItem->itemState & ODS_DISABLED) != 0;

    const BOOL pressed =
        (drawItem->itemState & ODS_SELECTED) != 0;

    const BOOL focused =
        (drawItem->itemState & ODS_FOCUS) != 0;

    const BOOL checked =
        GetImportCheckboxState(
            drawItem->hwndItem);

    const COLORREF backgroundColor =
        GetSysColor(COLOR_BTNFACE);

    HBRUSH backgroundBrush =
        CreateSolidBrush(backgroundColor);

    FillRect(
        deviceContext,
        &itemRect,
        backgroundBrush);

    DeleteObject(backgroundBrush);

    RECT boxRect;
    boxRect.left = itemRect.left + 3;
    boxRect.top =
        itemRect.top +
        ((itemRect.bottom - itemRect.top - 15) / 2);
    boxRect.right = boxRect.left + 15;
    boxRect.bottom = boxRect.top + 15;

    HBRUSH boxBrush =
        CreateSolidBrush(
            disabled
                ? RGB(238, 238, 238)
                : RGB(255, 255, 255));

    FillRect(
        deviceContext,
        &boxRect,
        boxBrush);

    DeleteObject(boxBrush);

    HPEN borderPen =
        CreatePen(
            PS_SOLID,
            1,
            disabled
                ? RGB(145, 145, 145)
                : RGB(70, 70, 70));

    HPEN oldPen =
        reinterpret_cast<HPEN>(
            SelectObject(
                deviceContext,
                borderPen));

    HBRUSH oldBrush =
        reinterpret_cast<HBRUSH>(
            SelectObject(
                deviceContext,
                GetStockObject(NULL_BRUSH)));

    Rectangle(
        deviceContext,
        boxRect.left,
        boxRect.top,
        boxRect.right,
        boxRect.bottom);

    if (checked)
    {
        HPEN checkPen =
            CreatePen(
                PS_SOLID,
                2,
                disabled
                    ? RGB(135, 135, 135)
                    : RGB(20, 20, 20));

        SelectObject(
            deviceContext,
            checkPen);

        const int offset =
            pressed ? 1 : 0;

        MoveToEx(
            deviceContext,
            boxRect.left + 3 + offset,
            boxRect.top + 7 + offset,
            NULL);

        LineTo(
            deviceContext,
            boxRect.left + 6 + offset,
            boxRect.top + 11 + offset);

        LineTo(
            deviceContext,
            boxRect.left + 12 + offset,
            boxRect.top + 3 + offset);

        SelectObject(
            deviceContext,
            borderPen);

        DeleteObject(checkPen);
    }

    SelectObject(
        deviceContext,
        oldBrush);

    SelectObject(
        deviceContext,
        oldPen);

    DeleteObject(borderPen);

    TCHAR caption[256];
    caption[0] = _T('\0');

    GetWindowText(
        drawItem->hwndItem,
        caption,
        _countof(caption));

    RECT textRect =
        itemRect;

    textRect.left =
        boxRect.right + 7;

    SetBkMode(
        deviceContext,
        TRANSPARENT);

    SetTextColor(
        deviceContext,
        disabled
            ? GetSysColor(COLOR_GRAYTEXT)
            : RGB(20, 20, 20));

    DrawText(
        deviceContext,
        caption,
        -1,
        &textRect,
        DT_LEFT |
        DT_VCENTER |
        DT_SINGLELINE |
        DT_END_ELLIPSIS);

    if (focused &&
        !disabled)
    {
        RECT focusRect =
            textRect;

        focusRect.right -= 2;
        focusRect.top += 2;
        focusRect.bottom -= 2;

        DrawFocusRect(
            deviceContext,
            &focusRect);
    }
}


static void DrawLightImportPushButton(
    const DRAWITEMSTRUCT* drawItem)
{
    if (drawItem == NULL)
    {
        return;
    }

    HDC deviceContext =
        drawItem->hDC;

    RECT itemRect =
        drawItem->rcItem;

    const BOOL disabled =
        (drawItem->itemState & ODS_DISABLED) != 0;

    const BOOL pressed =
        (drawItem->itemState & ODS_SELECTED) != 0;

    const BOOL focused =
        (drawItem->itemState & ODS_FOCUS) != 0;

    const int controlId =
        GetDlgCtrlID(
            drawItem->hwndItem);

    const BOOL defaultButton =
        controlId == MAXGLB_ID_IMPORT ||
        controlId == MAXGLB_ID_EXPORT_CONFIRM;

    HBRUSH faceBrush =
        CreateSolidBrush(
            pressed
                ? RGB(218, 218, 218)
                : RGB(245, 245, 245));

    FillRect(
        deviceContext,
        &itemRect,
        faceBrush);

    DeleteObject(faceBrush);

    HPEN borderPen =
        CreatePen(
            PS_SOLID,
            defaultButton ? 2 : 1,
            disabled
                ? RGB(155, 155, 155)
                : RGB(65, 65, 65));

    HPEN oldPen =
        reinterpret_cast<HPEN>(
            SelectObject(
                deviceContext,
                borderPen));

    HBRUSH oldBrush =
        reinterpret_cast<HBRUSH>(
            SelectObject(
                deviceContext,
                GetStockObject(NULL_BRUSH)));

    Rectangle(
        deviceContext,
        itemRect.left,
        itemRect.top,
        itemRect.right,
        itemRect.bottom);

    SelectObject(
        deviceContext,
        oldBrush);

    SelectObject(
        deviceContext,
        oldPen);

    DeleteObject(borderPen);

    const BOOL isArrowButton =
        controlId == MAXGLB_ID_PREVIEW_ROTATE_UP ||
        controlId == MAXGLB_ID_PREVIEW_ROTATE_DOWN ||
        controlId == MAXGLB_ID_PREVIEW_ROTATE_LEFT ||
        controlId == MAXGLB_ID_PREVIEW_ROTATE_RIGHT;

    if (isArrowButton)
    {
        const int offset =
            pressed ? 1 : 0;

        const int centerX =
            (itemRect.left + itemRect.right) / 2 +
            offset;

        const int centerY =
            (itemRect.top + itemRect.bottom) / 2 +
            offset;

        HPEN arrowPen =
            CreatePen(
                PS_SOLID,
                2,
                disabled
                    ? GetSysColor(COLOR_GRAYTEXT)
                    : RGB(20, 20, 20));

        HPEN oldArrowPen =
            reinterpret_cast<HPEN>(
                SelectObject(
                    deviceContext,
                    arrowPen));

        if (controlId == MAXGLB_ID_PREVIEW_ROTATE_UP)
        {
            MoveToEx(deviceContext, centerX, centerY + 7, NULL);
            LineTo(deviceContext, centerX, centerY - 7);
            LineTo(deviceContext, centerX - 5, centerY - 2);

            MoveToEx(deviceContext, centerX, centerY - 7, NULL);
            LineTo(deviceContext, centerX + 5, centerY - 2);
        }
        else if (controlId == MAXGLB_ID_PREVIEW_ROTATE_DOWN)
        {
            MoveToEx(deviceContext, centerX, centerY - 7, NULL);
            LineTo(deviceContext, centerX, centerY + 7);
            LineTo(deviceContext, centerX - 5, centerY + 2);

            MoveToEx(deviceContext, centerX, centerY + 7, NULL);
            LineTo(deviceContext, centerX + 5, centerY + 2);
        }
        else if (controlId == MAXGLB_ID_PREVIEW_ROTATE_LEFT)
        {
            MoveToEx(deviceContext, centerX + 7, centerY, NULL);
            LineTo(deviceContext, centerX - 7, centerY);
            LineTo(deviceContext, centerX - 2, centerY - 5);

            MoveToEx(deviceContext, centerX - 7, centerY, NULL);
            LineTo(deviceContext, centerX - 2, centerY + 5);
        }
        else
        {
            MoveToEx(deviceContext, centerX - 7, centerY, NULL);
            LineTo(deviceContext, centerX + 7, centerY);
            LineTo(deviceContext, centerX + 2, centerY - 5);

            MoveToEx(deviceContext, centerX + 7, centerY, NULL);
            LineTo(deviceContext, centerX + 2, centerY + 5);
        }

        SelectObject(
            deviceContext,
            oldArrowPen);

        DeleteObject(arrowPen);

        if (focused &&
            !disabled)
        {
            RECT focusRect =
                itemRect;

            InflateRect(
                &focusRect,
                -4,
                -4);

            DrawFocusRect(
                deviceContext,
                &focusRect);
        }

        return;
    }

    TCHAR caption[128];
    caption[0] = _T('\0');

    GetWindowText(
        drawItem->hwndItem,
        caption,
        _countof(caption));

    RECT textRect =
        itemRect;

    if (pressed)
    {
        ++textRect.left;
        ++textRect.top;
        ++textRect.right;
        ++textRect.bottom;
    }

    SetBkMode(
        deviceContext,
        TRANSPARENT);

    SetTextColor(
        deviceContext,
        disabled
            ? GetSysColor(COLOR_GRAYTEXT)
            : RGB(20, 20, 20));

    DrawText(
        deviceContext,
        caption,
        -1,
        &textRect,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE);

    if (focused &&
        !disabled)
    {
        RECT focusRect =
            itemRect;

        InflateRect(
            &focusRect,
            -4,
            -4);

        DrawFocusRect(
            deviceContext,
            &focusRect);
    }
}


static void SetImportControlFont(
    HWND control)
{
    if (control != NULL)
    {
        SendMessage(
            control,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(
                GetStockObject(DEFAULT_GUI_FONT)),
            TRUE);
    }
}


static HWND CreateImportControl(
    DWORD extendedStyle,
    const TCHAR* className,
    const TCHAR* text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    HWND parent,
    int controlId,
    LPVOID createParameter)
{
    HWND control =
        CreateWindowEx(
            extendedStyle,
            className,
            text,
            style,
            x,
            y,
            width,
            height,
            parent,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(controlId)),
            hInstance,
            createParameter);

    SetImportControlFont(control);
    return control;
}


static int CountImportablePrimitivesRecursive(
    const cgltf_node* node)
{
    if (node == NULL)
    {
        return 0;
    }

    int count = 0;

    if (node->mesh != NULL)
    {
        for (cgltf_size primitiveIndex = 0;
             primitiveIndex < node->mesh->primitives_count;
             ++primitiveIndex)
        {
            const cgltf_primitive* primitive =
                &node->mesh->primitives[primitiveIndex];

            const cgltf_accessor* positions =
                cgltf_find_accessor(
                    primitive,
                    cgltf_attribute_type_position,
                    0);

            if (primitive->type == cgltf_primitive_type_triangles &&
                positions != NULL &&
                positions->count > 0)
            {
                ++count;
            }
        }
    }

    for (cgltf_size childIndex = 0;
         childIndex < node->children_count;
         ++childIndex)
    {
        count +=
            CountImportablePrimitivesRecursive(
                node->children[childIndex]);
    }

    return count;
}


static int CountImportableScenePrimitives(
    const cgltf_data* data)
{
    if (data == NULL)
    {
        return 0;
    }

    int count = 0;

    const cgltf_scene* activeScene =
        data->scene;

    if (activeScene == NULL &&
        data->scenes_count > 0)
    {
        activeScene =
            &data->scenes[0];
    }

    if (activeScene != NULL &&
        activeScene->nodes_count > 0)
    {
        for (cgltf_size rootIndex = 0;
             rootIndex < activeScene->nodes_count;
             ++rootIndex)
        {
            count +=
                CountImportablePrimitivesRecursive(
                    activeScene->nodes[rootIndex]);
        }
    }
    else
    {
        for (cgltf_size nodeIndex = 0;
             nodeIndex < data->nodes_count;
             ++nodeIndex)
        {
            const cgltf_node* node =
                &data->nodes[nodeIndex];

            if (FindParentNode(data, node) == NULL)
            {
                count +=
                    CountImportablePrimitivesRecursive(
                        node);
            }
        }
    }

    return count;
}


static int CountImportableMeshNodesRecursive(
    const cgltf_node* node)
{
    if (node == NULL)
    {
        return 0;
    }

    int count = 0;

    if (node->mesh != NULL)
    {
        BOOL hasImportablePrimitive = FALSE;

        for (cgltf_size primitiveIndex = 0;
             primitiveIndex < node->mesh->primitives_count;
             ++primitiveIndex)
        {
            const cgltf_primitive* primitive =
                &node->mesh->primitives[primitiveIndex];

            const cgltf_accessor* positions =
                cgltf_find_accessor(
                    primitive,
                    cgltf_attribute_type_position,
                    0);

            if (primitive->type ==
                    cgltf_primitive_type_triangles &&
                positions != NULL &&
                positions->type ==
                    cgltf_type_vec3 &&
                positions->count > 0)
            {
                hasImportablePrimitive =
                    TRUE;
                break;
            }
        }

        if (hasImportablePrimitive)
        {
            ++count;
        }
    }

    for (cgltf_size childIndex = 0;
         childIndex < node->children_count;
         ++childIndex)
    {
        count +=
            CountImportableMeshNodesRecursive(
                node->children[childIndex]);
    }

    return count;
}


static int CountImportableSceneMeshNodes(
    const cgltf_data* data)
{
    if (data == NULL)
    {
        return 0;
    }

    int count = 0;

    const cgltf_scene* activeScene =
        data->scene;

    if (activeScene == NULL &&
        data->scenes_count > 0)
    {
        activeScene =
            &data->scenes[0];
    }

    if (activeScene != NULL &&
        activeScene->nodes_count > 0)
    {
        for (cgltf_size rootIndex = 0;
             rootIndex < activeScene->nodes_count;
             ++rootIndex)
        {
            count +=
                CountImportableMeshNodesRecursive(
                    activeScene->nodes[rootIndex]);
        }
    }
    else
    {
        for (cgltf_size nodeIndex = 0;
             nodeIndex < data->nodes_count;
             ++nodeIndex)
        {
            const cgltf_node* node =
                &data->nodes[nodeIndex];

            if (FindParentNode(data, node) == NULL)
            {
                count +=
                    CountImportableMeshNodesRecursive(
                        node);
            }
        }
    }

    return count;
}


static void AddNodeTreeLinesRecursive(
    HWND listBox,
    const cgltf_node* node,
    int depth)
{
    if (listBox == NULL ||
        node == NULL)
    {
        return;
    }

    TCHAR nodeName[256];
    ConvertUtf8ToTchar(
        node->name,
        nodeName,
        _countof(nodeName));

    if (nodeName[0] == _T('\0'))
    {
        _tcscpy_s(
            nodeName,
            _countof(nodeName),
            _T("(unnamed node)"));
    }

    TCHAR indentation[64];
    indentation[0] = _T('\0');

    const int indentationCount =
        depth < 15
        ? depth
        : 15;

    for (int indentationIndex = 0;
         indentationIndex < indentationCount;
         ++indentationIndex)
    {
        _tcscat_s(
            indentation,
            _countof(indentation),
            _T("  "));
    }

    TCHAR line[384];

    if (node->mesh != NULL)
    {
        _stprintf_s(
            line,
            _countof(line),
            _T("%s%s  [mesh: %u primitive%s]"),
            indentation,
            nodeName,
            static_cast<unsigned int>(
                node->mesh->primitives_count),
            node->mesh->primitives_count == 1
                ? _T("")
                : _T("s"));
    }
    else
    {
        _stprintf_s(
            line,
            _countof(line),
            _T("%s%s"),
            indentation,
            nodeName);
    }

    SendMessage(
        listBox,
        LB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(line));

    for (cgltf_size childIndex = 0;
         childIndex < node->children_count;
         ++childIndex)
    {
        AddNodeTreeLinesRecursive(
            listBox,
            node->children[childIndex],
            depth + 1);
    }
}


static void PopulateObjectTreeList(
    HWND listBox,
    const cgltf_data* data)
{
    if (listBox == NULL ||
        data == NULL)
    {
        return;
    }

    const cgltf_scene* activeScene =
        data->scene;

    if (activeScene == NULL &&
        data->scenes_count > 0)
    {
        activeScene =
            &data->scenes[0];
    }

    if (activeScene != NULL &&
        activeScene->nodes_count > 0)
    {
        for (cgltf_size rootIndex = 0;
             rootIndex < activeScene->nodes_count;
             ++rootIndex)
        {
            AddNodeTreeLinesRecursive(
                listBox,
                activeScene->nodes[rootIndex],
                0);
        }
    }
    else
    {
        for (cgltf_size nodeIndex = 0;
             nodeIndex < data->nodes_count;
             ++nodeIndex)
        {
            const cgltf_node* node =
                &data->nodes[nodeIndex];

            if (FindParentNode(data, node) == NULL)
            {
                AddNodeTreeLinesRecursive(
                    listBox,
                    node,
                    0);
            }
        }
    }
}


static void PopulateMaterialList(
    HWND listBox,
    const cgltf_data* data)
{
    if (listBox == NULL ||
        data == NULL)
    {
        return;
    }

    if (data->materials_count == 0)
    {
        SendMessage(
            listBox,
            LB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(
                _T("(no materials)")));
        return;
    }

    for (cgltf_size materialIndex = 0;
         materialIndex < data->materials_count;
         ++materialIndex)
    {
        TCHAR materialName[256];
        ConvertUtf8ToTchar(
            data->materials[materialIndex].name,
            materialName,
            _countof(materialName));

        if (materialName[0] == _T('\0'))
        {
            _stprintf_s(
                materialName,
                _countof(materialName),
                _T("Material %u"),
                static_cast<unsigned int>(
                    materialIndex));
        }

        SendMessage(
            listBox,
            LB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(
                materialName));
    }
}


static void PopulateTextureList(
    HWND listBox,
    const cgltf_data* data)
{
    if (listBox == NULL ||
        data == NULL)
    {
        return;
    }

    if (data->images_count == 0)
    {
        SendMessage(
            listBox,
            LB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(
                _T("(no textures)")));
        return;
    }

    for (cgltf_size imageIndex = 0;
         imageIndex < data->images_count;
         ++imageIndex)
    {
        const cgltf_image& image =
            data->images[imageIndex];

        TCHAR imageName[256];
        ConvertUtf8ToTchar(
            image.name,
            imageName,
            _countof(imageName));

        if (imageName[0] == _T('\0'))
        {
            ConvertUtf8ToTchar(
                image.uri,
                imageName,
                _countof(imageName));
        }

        if (imageName[0] == _T('\0'))
        {
            _stprintf_s(
                imageName,
                _countof(imageName),
                _T("Embedded image %u"),
                static_cast<unsigned int>(
                    imageIndex));
        }

        SendMessage(
            listBox,
            LB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(
                imageName));
    }
}


static void CollectPreviewPointsFromNode(
    const cgltf_data* data,
    const cgltf_node* node,
    std::vector<Point3>& outputPoints,
    size_t maximumPointCount)
{
    if (data == NULL ||
        node == NULL ||
        outputPoints.size() >= maximumPointCount)
    {
        return;
    }

    if (node->mesh != NULL)
    {
        cgltf_float worldMatrix[16];
        BuildGltfWorldMatrix(
            data,
            node,
            worldMatrix);

        for (cgltf_size primitiveIndex = 0;
             primitiveIndex < node->mesh->primitives_count &&
             outputPoints.size() < maximumPointCount;
             ++primitiveIndex)
        {
            const cgltf_primitive* primitive =
                &node->mesh->primitives[primitiveIndex];

            if (primitive->type !=
                cgltf_primitive_type_triangles)
            {
                continue;
            }

            const cgltf_accessor* positions =
                cgltf_find_accessor(
                    primitive,
                    cgltf_attribute_type_position,
                    0);

            if (positions == NULL ||
                positions->count == 0)
            {
                continue;
            }

            cgltf_size sampleStep = 1;

            const size_t remainingCapacity =
                maximumPointCount -
                outputPoints.size();

            if (positions->count > remainingCapacity &&
                remainingCapacity > 0)
            {
                sampleStep =
                    positions->count /
                    remainingCapacity;

                if (sampleStep < 1)
                {
                    sampleStep = 1;
                }
            }

            for (cgltf_size vertexIndex = 0;
                 vertexIndex < positions->count &&
                 outputPoints.size() < maximumPointCount;
                 vertexIndex += sampleStep)
            {
                cgltf_float localPosition[3];

                if (!cgltf_accessor_read_float(
                        positions,
                        vertexIndex,
                        localPosition,
                        3))
                {
                    continue;
                }

                cgltf_float worldPosition[3];

                TransformGltfPoint(
                    worldMatrix,
                    localPosition[0],
                    localPosition[1],
                    localPosition[2],
                    worldPosition);

                outputPoints.push_back(
                    ConvertGltfPositionToMax(
                        worldPosition,
                        NULL));
            }
        }
    }

    for (cgltf_size childIndex = 0;
         childIndex < node->children_count &&
         outputPoints.size() < maximumPointCount;
         ++childIndex)
    {
        CollectPreviewPointsFromNode(
            data,
            node->children[childIndex],
            outputPoints,
            maximumPointCount);
    }
}


static void CollectScenePreviewPoints(
    const cgltf_data* data,
    std::vector<Point3>& outputPoints)
{
    outputPoints.clear();

    if (data == NULL)
    {
        return;
    }

    const size_t maximumPointCount =
        12000;

    const cgltf_scene* activeScene =
        data->scene;

    if (activeScene == NULL &&
        data->scenes_count > 0)
    {
        activeScene =
            &data->scenes[0];
    }

    if (activeScene != NULL &&
        activeScene->nodes_count > 0)
    {
        for (cgltf_size rootIndex = 0;
             rootIndex < activeScene->nodes_count &&
             outputPoints.size() < maximumPointCount;
             ++rootIndex)
        {
            CollectPreviewPointsFromNode(
                data,
                activeScene->nodes[rootIndex],
                outputPoints,
                maximumPointCount);
        }
    }
    else
    {
        for (cgltf_size nodeIndex = 0;
             nodeIndex < data->nodes_count &&
             outputPoints.size() < maximumPointCount;
             ++nodeIndex)
        {
            const cgltf_node* node =
                &data->nodes[nodeIndex];

            if (FindParentNode(data, node) == NULL)
            {
                CollectPreviewPointsFromNode(
                    data,
                    node,
                    outputPoints,
                    maximumPointCount);
            }
        }
    }
}


static void ExpandImportBoundsFromNode(
    const cgltf_data* data,
    const cgltf_node* node,
    const float* correctionMatrix,
    Point3* minimum,
    Point3* maximum,
    BOOL* hasBounds)
{
    if (data == NULL ||
        node == NULL ||
        minimum == NULL ||
        maximum == NULL ||
        hasBounds == NULL)
    {
        return;
    }

    cgltf_float worldMatrix[16];
    BuildGltfWorldMatrix(
        data,
        node,
        worldMatrix);

    if (node->mesh != NULL)
    {
        for (cgltf_size primitiveIndex = 0;
             primitiveIndex < node->mesh->primitives_count;
             ++primitiveIndex)
        {
            const cgltf_primitive* primitive =
                &node->mesh->primitives[primitiveIndex];

            if (primitive->type !=
                cgltf_primitive_type_triangles)
            {
                continue;
            }

            const cgltf_accessor* positions =
                cgltf_find_accessor(
                    primitive,
                    cgltf_attribute_type_position,
                    0);

            if (positions == NULL)
            {
                continue;
            }

            for (cgltf_size vertexIndex = 0;
                 vertexIndex < positions->count;
                 ++vertexIndex)
            {
                cgltf_float localPosition[3];

                if (!cgltf_accessor_read_float(
                        positions,
                        vertexIndex,
                        localPosition,
                        3))
                {
                    continue;
                }

                cgltf_float worldPosition[3];

                TransformGltfPoint(
                    worldMatrix,
                    localPosition[0],
                    localPosition[1],
                    localPosition[2],
                    worldPosition);

                const Point3 converted =
                    ConvertGltfPositionToMax(
                        worldPosition,
                        correctionMatrix);

                if (!*hasBounds)
                {
                    *minimum = converted;
                    *maximum = converted;
                    *hasBounds = TRUE;
                }
                else
                {
                    minimum->x =
                        converted.x < minimum->x
                        ? converted.x
                        : minimum->x;

                    minimum->y =
                        converted.y < minimum->y
                        ? converted.y
                        : minimum->y;

                    minimum->z =
                        converted.z < minimum->z
                        ? converted.z
                        : minimum->z;

                    maximum->x =
                        converted.x > maximum->x
                        ? converted.x
                        : maximum->x;

                    maximum->y =
                        converted.y > maximum->y
                        ? converted.y
                        : maximum->y;

                    maximum->z =
                        converted.z > maximum->z
                        ? converted.z
                        : maximum->z;
                }
            }
        }
    }

    for (cgltf_size childIndex = 0;
         childIndex < node->children_count;
         ++childIndex)
    {
        ExpandImportBoundsFromNode(
            data,
            node->children[childIndex],
            correctionMatrix,
            minimum,
            maximum,
            hasBounds);
    }
}


static BOOL ComputeNormalizedImportScale(
    const cgltf_data* data,
    const float* correctionMatrix,
    float targetSize,
    float* outputScale)
{
    if (data == NULL ||
        outputScale == NULL ||
        targetSize <= 0.0f)
    {
        return FALSE;
    }

    Point3 minimum(0.0f, 0.0f, 0.0f);
    Point3 maximum(0.0f, 0.0f, 0.0f);
    BOOL hasBounds = FALSE;

    const cgltf_scene* activeScene =
        data->scene;

    if (activeScene == NULL &&
        data->scenes_count > 0)
    {
        activeScene =
            &data->scenes[0];
    }

    if (activeScene != NULL &&
        activeScene->nodes_count > 0)
    {
        for (cgltf_size rootIndex = 0;
             rootIndex < activeScene->nodes_count;
             ++rootIndex)
        {
            ExpandImportBoundsFromNode(
                data,
                activeScene->nodes[rootIndex],
                correctionMatrix,
                &minimum,
                &maximum,
                &hasBounds);
        }
    }
    else
    {
        for (cgltf_size nodeIndex = 0;
             nodeIndex < data->nodes_count;
             ++nodeIndex)
        {
            const cgltf_node* node =
                &data->nodes[nodeIndex];

            if (FindParentNode(data, node) == NULL)
            {
                ExpandImportBoundsFromNode(
                    data,
                    node,
                    correctionMatrix,
                    &minimum,
                    &maximum,
                    &hasBounds);
            }
        }
    }

    if (!hasBounds)
    {
        return FALSE;
    }

    const float extentX =
        maximum.x - minimum.x;

    const float extentY =
        maximum.y - minimum.y;

    const float extentZ =
        maximum.z - minimum.z;

    float largestExtent = extentX;

    if (extentY > largestExtent)
    {
        largestExtent = extentY;
    }

    if (extentZ > largestExtent)
    {
        largestExtent = extentZ;
    }

    if (largestExtent <= 1.0e-20f)
    {
        return FALSE;
    }

    *outputScale =
        targetSize /
        largestExtent;

    return *outputScale > 0.0f &&
        *outputScale == *outputScale;
}


static void DrawImportPreview(
    HWND window,
    HDC deviceContext,
    const MaxGLBImportDialogContext* context)
{
    RECT clientRect;
    GetClientRect(
        window,
        &clientRect);

    HBRUSH backgroundBrush =
        CreateSolidBrush(
            RGB(38, 42, 48));

    FillRect(
        deviceContext,
        &clientRect,
        backgroundBrush);

    DeleteObject(backgroundBrush);

    FrameRect(
        deviceContext,
        &clientRect,
        reinterpret_cast<HBRUSH>(
            GetStockObject(GRAY_BRUSH)));

    if (context == NULL ||
        context->previewPoints.empty())
    {
        SetBkMode(
            deviceContext,
            TRANSPARENT);

        SetTextColor(
            deviceContext,
            RGB(220, 220, 220));

        DrawText(
            deviceContext,
            _T("No geometry preview"),
            -1,
            &clientRect,
            DT_CENTER |
            DT_VCENTER |
            DT_SINGLELINE);

        return;
    }

    float minimumX = FLT_MAX;
    float minimumY = FLT_MAX;
    float maximumX = -FLT_MAX;
    float maximumY = -FLT_MAX;

    std::vector<Point2> projectedPoints;
    projectedPoints.reserve(
        context->previewPoints.size());

    for (size_t pointIndex = 0;
         pointIndex < context->previewPoints.size();
         ++pointIndex)
    {
        const Point3 corrected =
            ApplyImportRotationMatrix(
                context->previewPoints[pointIndex],
                context->settings.rotationMatrix);

        // True 3ds Max Front view: X runs horizontally and Z vertically.
        // We look along the negative Y axis. This makes "up" unambiguous
        // when choosing an additional X-axis correction.
        const float projectedX =
            corrected.x;

        const float projectedY =
            corrected.z;

        projectedPoints.push_back(
            Point2(projectedX, projectedY));

        if (projectedX < minimumX)
        {
            minimumX = projectedX;
        }

        if (projectedX > maximumX)
        {
            maximumX = projectedX;
        }

        if (projectedY < minimumY)
        {
            minimumY = projectedY;
        }

        if (projectedY > maximumY)
        {
            maximumY = projectedY;
        }
    }

    const float rangeX =
        maximumX - minimumX;

    const float rangeY =
        maximumY - minimumY;

    const int clientWidth =
        clientRect.right - clientRect.left;

    const int clientHeight =
        clientRect.bottom - clientRect.top;

    const float availableWidth =
        static_cast<float>(clientWidth - 14);

    const float availableHeight =
        static_cast<float>(clientHeight - 38);

    float scale = 1.0f;

    if (rangeX > 1.0e-6f ||
        rangeY > 1.0e-6f)
    {
        const float scaleX =
            rangeX > 1.0e-6f
            ? availableWidth / rangeX
            : FLT_MAX;

        const float scaleY =
            rangeY > 1.0e-6f
            ? availableHeight / rangeY
            : FLT_MAX;

        scale =
            scaleX < scaleY
            ? scaleX
            : scaleY;
    }

    const float centerX =
        (minimumX + maximumX) * 0.5f;

    const float centerY =
        (minimumY + maximumY) * 0.5f;

    const COLORREF pointColor =
        RGB(225, 231, 238);

    for (size_t pointIndex = 0;
         pointIndex < projectedPoints.size();
         ++pointIndex)
    {
        const int screenX =
            clientWidth / 2 +
            static_cast<int>(
                (projectedPoints[pointIndex].x - centerX) *
                scale);

        const int screenY =
            (clientHeight - 8) / 2 -
            static_cast<int>(
                (projectedPoints[pointIndex].y - centerY) *
                scale);

        if (screenX > 1 &&
            screenX < clientWidth - 1 &&
            screenY > 1 &&
            screenY < clientHeight - 28)
        {
            SetPixelV(
                deviceContext,
                screenX,
                screenY,
                pointColor);

            if (screenX + 1 < clientWidth - 1)
            {
                SetPixelV(
                    deviceContext,
                    screenX + 1,
                    screenY,
                    pointColor);
            }
        }
    }

    // Small orientation indicator: Max Front view uses X horizontally and
    // Z vertically. It remains fixed while the model preview rotates.
    HPEN axisPen =
        CreatePen(
            PS_SOLID,
            1,
            RGB(126, 205, 126));

    HPEN oldPen =
        reinterpret_cast<HPEN>(
            SelectObject(
                deviceContext,
                axisPen));

    const int axisOriginX = 13;
    const int axisOriginY = clientHeight - 34;

    MoveToEx(
        deviceContext,
        axisOriginX,
        axisOriginY,
        NULL);

    LineTo(
        deviceContext,
        axisOriginX,
        13);

    LineTo(
        deviceContext,
        axisOriginX - 3,
        19);

    MoveToEx(
        deviceContext,
        axisOriginX,
        13,
        NULL);

    LineTo(
        deviceContext,
        axisOriginX + 3,
        19);

    MoveToEx(
        deviceContext,
        axisOriginX,
        axisOriginY,
        NULL);

    LineTo(
        deviceContext,
        34,
        axisOriginY);

    LineTo(
        deviceContext,
        28,
        axisOriginY - 3);

    MoveToEx(
        deviceContext,
        34,
        axisOriginY,
        NULL);

    LineTo(
        deviceContext,
        28,
        axisOriginY + 3);

    SelectObject(
        deviceContext,
        oldPen);

    DeleteObject(axisPen);

    SetBkMode(
        deviceContext,
        TRANSPARENT);

    SetTextColor(
        deviceContext,
        RGB(151, 224, 151));

    RECT zLabelRect;
    zLabelRect.left = 17;
    zLabelRect.top = 4;
    zLabelRect.right = 46;
    zLabelRect.bottom = 22;

    DrawText(
        deviceContext,
        _T("Z+"),
        -1,
        &zLabelRect,
        DT_LEFT |
        DT_VCENTER |
        DT_SINGLELINE);

    RECT xLabelRect;
    xLabelRect.left = 35;
    xLabelRect.top = axisOriginY - 9;
    xLabelRect.right = 62;
    xLabelRect.bottom = axisOriginY + 9;

    DrawText(
        deviceContext,
        _T("X+"),
        -1,
        &xLabelRect,
        DT_LEFT |
        DT_VCENTER |
        DT_SINGLELINE);

    TCHAR rotationText[96];
    _tcscpy_s(
        rotationText,
        _countof(rotationText),
        _T("Front X/Z  |  arrows rotate import"));

    RECT textRect = clientRect;
    textRect.top =
        clientRect.bottom - 24;

    SetBkMode(
        deviceContext,
        TRANSPARENT);

    SetTextColor(
        deviceContext,
        RGB(190, 198, 208));

    DrawText(
        deviceContext,
        rotationText,
        -1,
        &textRect,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE);
}


static LRESULT CALLBACK MaxGLBPreviewWindowProcedure(
    HWND window,
    UINT message,
    WPARAM wordParameter,
    LPARAM longParameter)
{
    MaxGLBImportDialogContext* context =
        reinterpret_cast<MaxGLBImportDialogContext*>(
            GetWindowLongPtr(
                window,
                GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
        CREATESTRUCT* createStruct =
            reinterpret_cast<CREATESTRUCT*>(
                longParameter);

        context =
            reinterpret_cast<MaxGLBImportDialogContext*>(
                createStruct->lpCreateParams);

        SetWindowLongPtr(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(context));
    }

    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        {
            PAINTSTRUCT paintStruct;
            HDC deviceContext =
                BeginPaint(
                    window,
                    &paintStruct);

            DrawImportPreview(
                window,
                deviceContext,
                context);

            EndPaint(
                window,
                &paintStruct);
        }
        return 0;
    }

    return DefWindowProc(
        window,
        message,
        wordParameter,
        longParameter);
}


static BOOL TryReadUnitFloat(
    HWND editControl,
    float* outputValue)
{
    if (editControl == NULL ||
        outputValue == NULL)
    {
        return FALSE;
    }

    TCHAR textValue[64];

    GetWindowText(
        editControl,
        textValue,
        _countof(textValue));

    TCHAR* endPointer = NULL;

    const double parsedValue =
        _tcstod(
            textValue,
            &endPointer);

    if (endPointer == textValue ||
        parsedValue < 0.0 ||
        parsedValue > 1.0)
    {
        return FALSE;
    }

    *outputValue =
        static_cast<float>(
            parsedValue);

    return TRUE;
}


static BOOL TryReadPositiveImportSize(
    HWND editControl,
    float* outputValue)
{
    if (editControl == NULL ||
        outputValue == NULL)
    {
        return FALSE;
    }

    TCHAR text[64];
    GetWindowText(
        editControl,
        text,
        _countof(text));

    TCHAR* endPointer = NULL;
    const double parsedValue =
        _tcstod(
            text,
            &endPointer);

    if (endPointer == text)
    {
        return FALSE;
    }

    while (endPointer != NULL &&
           (*endPointer == _T(' ') ||
            *endPointer == _T('\t')))
    {
        ++endPointer;
    }

    if (endPointer == NULL ||
        *endPointer != _T('\0') ||
        parsedValue != parsedValue ||
        parsedValue <= 0.0 ||
        parsedValue > 1000000000.0)
    {
        return FALSE;
    }

    *outputValue =
        static_cast<float>(parsedValue);

    return TRUE;
}


static void GetFilenameOnly(
    const TCHAR* fullPath,
    TCHAR* outputName,
    size_t outputNameCount)
{
    if (outputName == NULL ||
        outputNameCount == 0)
    {
        return;
    }

    outputName[0] = _T('\0');

    if (fullPath == NULL)
    {
        return;
    }

    const TCHAR* lastBackslash =
        _tcsrchr(
            fullPath,
            _T('\\'));

    const TCHAR* lastForwardSlash =
        _tcsrchr(
            fullPath,
            _T('/'));

    const TCHAR* filename =
        fullPath;

    if (lastBackslash != NULL &&
        lastBackslash + 1 > filename)
    {
        filename =
            lastBackslash + 1;
    }

    if (lastForwardSlash != NULL &&
        lastForwardSlash + 1 > filename)
    {
        filename =
            lastForwardSlash + 1;
    }

    _tcscpy_s(
        outputName,
        outputNameCount,
        filename);
}


static LRESULT CALLBACK MaxGLBImportWindowProcedure(
    HWND window,
    UINT message,
    WPARAM wordParameter,
    LPARAM longParameter)
{
    MaxGLBImportDialogContext* context =
        reinterpret_cast<MaxGLBImportDialogContext*>(
            GetWindowLongPtr(
                window,
                GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
        CREATESTRUCT* createStruct =
            reinterpret_cast<CREATESTRUCT*>(
                longParameter);

        context =
            reinterpret_cast<MaxGLBImportDialogContext*>(
                createStruct->lpCreateParams);

        SetWindowLongPtr(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(context));
    }

    switch (message)
    {
    case WM_CREATE:
        {
            if (context == NULL)
            {
                return -1;
            }

            context->window =
                window;

            TCHAR filenameOnly[MAX_PATH];
            GetFilenameOnly(
                context->filename,
                filenameOnly,
                _countof(filenameOnly));

            TCHAR headerText[MAX_PATH + 64];
            _stprintf_s(
                headerText,
                _countof(headerText),
                _T("File: %s"),
                filenameOnly);

            CreateImportControl(
                0,
                _T("STATIC"),
                headerText,
                WS_CHILD |
                WS_VISIBLE |
                SS_LEFT,
                16,
                12,
                660,
                20,
                window,
                0,
                NULL);

            context->previewWindow =
                CreateImportControl(
                    0,
                    MAXGLB_PREVIEW_WINDOW_CLASS,
                    _T(""),
                    WS_CHILD |
                    WS_VISIBLE,
                    16,
                    40,
                    150,
                    150,
                    window,
                    0,
                    context);

            CreateImportControl(
                0,
                _T("STATIC"),
                _T("Import rotation"),
                WS_CHILD |
                WS_VISIBLE |
                SS_CENTER,
                24,
                194,
                134,
                18,
                window,
                0,
                NULL);

            context->previewRotateUpButton =
                CreateImportControl(
                    0,
                    _T("BUTTON"),
                    _T(""),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    BS_OWNERDRAW,
                    69,
                    214,
                    30,
                    25,
                    window,
                    MAXGLB_ID_PREVIEW_ROTATE_UP,
                    NULL);

            context->previewRotateLeftButton =
                CreateImportControl(
                    0,
                    _T("BUTTON"),
                    _T(""),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    BS_OWNERDRAW,
                    34,
                    243,
                    30,
                    25,
                    window,
                    MAXGLB_ID_PREVIEW_ROTATE_LEFT,
                    NULL);

            context->previewRotateResetButton =
                CreateImportControl(
                    0,
                    _T("BUTTON"),
                    _T("0"),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    BS_OWNERDRAW,
                    69,
                    243,
                    30,
                    25,
                    window,
                    MAXGLB_ID_PREVIEW_ROTATE_RESET,
                    NULL);

            context->previewRotateRightButton =
                CreateImportControl(
                    0,
                    _T("BUTTON"),
                    _T(""),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    BS_OWNERDRAW,
                    104,
                    243,
                    30,
                    25,
                    window,
                    MAXGLB_ID_PREVIEW_ROTATE_RIGHT,
                    NULL);

            context->previewRotateDownButton =
                CreateImportControl(
                    0,
                    _T("BUTTON"),
                    _T(""),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    BS_OWNERDRAW,
                    69,
                    272,
                    30,
                    25,
                    window,
                    MAXGLB_ID_PREVIEW_ROTATE_DOWN,
                    NULL);

            TCHAR summaryText[512];
            _stprintf_s(
                summaryText,
                _countof(summaryText),
                _T("Nodes: %u\r\n")
                _T("Meshes: %u\r\n")
                _T("Imported objects: %d\r\n")
                _T("Materials: %u\r\n")
                _T("Textures: %u\r\n")
                _T("Animations: %u"),
                static_cast<unsigned int>(
                    context->data->nodes_count),
                static_cast<unsigned int>(
                    context->data->meshes_count),
                context->importableObjectCount,
                static_cast<unsigned int>(
                    context->data->materials_count),
                static_cast<unsigned int>(
                    context->data->images_count),
                static_cast<unsigned int>(
                    context->data->animations_count));

            CreateImportControl(
                0,
                _T("STATIC"),
                summaryText,
                WS_CHILD |
                WS_VISIBLE |
                SS_LEFT,
                16,
                307,
                150,
                100,
                window,
                0,
                NULL);

            CreateImportControl(
                0,
                _T("STATIC"),
                _T("Object tree"),
                WS_CHILD |
                WS_VISIBLE,
                180,
                40,
                250,
                18,
                window,
                0,
                NULL);

            HWND objectList =
                CreateImportControl(
                    WS_EX_CLIENTEDGE,
                    _T("LISTBOX"),
                    _T(""),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_VSCROLL |
                    WS_HSCROLL |
                    LBS_NOINTEGRALHEIGHT,
                    180,
                    60,
                    250,
                    165,
                    window,
                    MAXGLB_ID_OBJECT_LIST,
                    NULL);

            PopulateObjectTreeList(
                objectList,
                context->data);

            CreateImportControl(
                0,
                _T("STATIC"),
                _T("Materials"),
                WS_CHILD |
                WS_VISIBLE,
                445,
                40,
                230,
                18,
                window,
                0,
                NULL);

            HWND materialList =
                CreateImportControl(
                    WS_EX_CLIENTEDGE,
                    _T("LISTBOX"),
                    _T(""),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_VSCROLL |
                    LBS_NOINTEGRALHEIGHT,
                    445,
                    60,
                    230,
                    70,
                    window,
                    MAXGLB_ID_MATERIAL_LIST,
                    NULL);

            PopulateMaterialList(
                materialList,
                context->data);

            CreateImportControl(
                0,
                _T("STATIC"),
                _T("Textures"),
                WS_CHILD |
                WS_VISIBLE,
                445,
                140,
                230,
                18,
                window,
                0,
                NULL);

            HWND textureList =
                CreateImportControl(
                    WS_EX_CLIENTEDGE,
                    _T("LISTBOX"),
                    _T(""),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_VSCROLL |
                    LBS_NOINTEGRALHEIGHT,
                    445,
                    160,
                    230,
                    65,
                    window,
                    MAXGLB_ID_TEXTURE_LIST,
                    NULL);

            PopulateTextureList(
                textureList,
                context->data);

            CreateImportControl(
                0,
                _T("STATIC"),
                _T("The arrows rotate the actual import in 90-degree steps. The center 0 resets it."),
                WS_CHILD |
                WS_VISIBLE |
                SS_LEFT,
                180,
                240,
                470,
                28,
                window,
                0,
                NULL);

            context->importTexturesCheck =
                CreateImportControl(
                    0,
                    _T("BUTTON"),
                    _T("Import textures"),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    BS_OWNERDRAW,
                    180,
                    280,
                    150,
                    24,
                    window,
                    MAXGLB_ID_IMPORT_TEXTURES,
                    NULL);

            UseReadableClassicTheme(
                context->importTexturesCheck);

            SetImportCheckboxState(
                context->importTexturesCheck,
                context->settings.importTextures);

            context->addParentCheck =
                CreateImportControl(
                    0,
                    _T("BUTTON"),
                    _T("Add parent node (Root:NAME)"),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    BS_OWNERDRAW,
                    340,
                    280,
                    220,
                    24,
                    window,
                    MAXGLB_ID_ADD_PARENT,
                    NULL);

            UseReadableClassicTheme(
                context->addParentCheck);

            SetImportCheckboxState(
                context->addParentCheck,
                context->settings.addParentNode &&
                context->importableObjectCount > 1);

            EnableWindow(
                context->addParentCheck,
                context->importableObjectCount > 1);

            InvalidateRect(
                context->addParentCheck,
                NULL,
                TRUE);

            context->normalizeSizeCheck =
                CreateImportControl(
                    0,
                    _T("BUTTON"),
                    _T("Normalize largest dimension to"),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    BS_OWNERDRAW,
                    180,
                    310,
                    205,
                    24,
                    window,
                    MAXGLB_ID_NORMALIZE_SIZE,
                    NULL);

            UseReadableClassicTheme(
                context->normalizeSizeCheck);

            SetImportCheckboxState(
                context->normalizeSizeCheck,
                context->settings.normalizeSize);

            TCHAR normalizeTargetText[64];
            _stprintf_s(
                normalizeTargetText,
                _countof(normalizeTargetText),
                _T("%.3f"),
                context->settings.normalizeTargetSize);

            context->normalizeTargetEdit =
                CreateImportControl(
                    WS_EX_CLIENTEDGE,
                    _T("EDIT"),
                    normalizeTargetText,
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    ES_AUTOHSCROLL,
                    395,
                    310,
                    80,
                    24,
                    window,
                    MAXGLB_ID_NORMALIZE_TARGET,
                    NULL);

            UseReadableClassicTheme(
                context->normalizeTargetEdit);

            EnableWindow(
                context->normalizeTargetEdit,
                context->settings.normalizeSize);

            CreateImportControl(
                0,
                _T("STATIC"),
                _T("Max units"),
                WS_CHILD |
                WS_VISIBLE,
                482,
                315,
                70,
                20,
                window,
                0,
                NULL);

            context->animationCheck =
                CreateImportControl(
                    0,
                    _T("BUTTON"),
                    _T("Import animation (not implemented)"),
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_OWNERDRAW,
                    180,
                    340,
                    230,
                    24,
                    window,
                    MAXGLB_ID_IMPORT_ANIMATION,
                    NULL);

            UseReadableClassicTheme(
                context->animationCheck);

            SetImportCheckboxState(
                context->animationCheck,
                FALSE);

            EnableWindow(
                context->animationCheck,
                FALSE);

            InvalidateRect(
                context->animationCheck,
                NULL,
                TRUE);

            CreateImportControl(
                0,
                _T("STATIC"),
                _T("Preview is the Max Front view: X is horizontal and Z+ is up.\r\n")
                _T("Use an X correction only when the model is sideways or upside down."),
                WS_CHILD |
                WS_VISIBLE |
                SS_LEFT,
                180,
                370,
                470,
                38,
                window,
                0,
                NULL);

            CreateImportControl(
                0,
                _T("BUTTON"),
                _T("Info"),
                WS_CHILD |
                WS_VISIBLE |
                WS_TABSTOP |
                BS_OWNERDRAW,
                16,
                415,
                80,
                28,
                window,
                MAXGLB_ID_INFO,
                NULL);

            CreateImportControl(
                0,
                _T("BUTTON"),
                _T("Cancel"),
                WS_CHILD |
                WS_VISIBLE |
                WS_TABSTOP |
                BS_OWNERDRAW,
                505,
                415,
                80,
                28,
                window,
                MAXGLB_ID_CANCEL,
                NULL);

            HWND importButton =
                CreateImportControl(
                    0,
                    _T("BUTTON"),
                    _T("Import"),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    BS_OWNERDRAW,
                    595,
                    415,
                    80,
                    28,
                    window,
                    MAXGLB_ID_IMPORT,
                    NULL);

            SendMessage(
                window,
                DM_SETDEFID,
                MAXGLB_ID_IMPORT,
                0);

            SetFocus(importButton);
        }
        return 0;

    case WM_CTLCOLOREDIT:
        {
            HDC controlDc =
                reinterpret_cast<HDC>(
                    wordParameter);

            HWND controlWindow =
                reinterpret_cast<HWND>(
                    longParameter);

            if (context != NULL &&
                controlWindow ==
                    context->normalizeTargetEdit)
            {
                if (IsWindowEnabled(controlWindow))
                {
                    SetTextColor(
                        controlDc,
                        GetSysColor(
                            COLOR_WINDOWTEXT));

                    SetBkColor(
                        controlDc,
                        GetSysColor(
                            COLOR_WINDOW));

                    return reinterpret_cast<LRESULT>(
                        GetSysColorBrush(
                            COLOR_WINDOW));
                }

                SetTextColor(
                    controlDc,
                    GetSysColor(
                        COLOR_GRAYTEXT));

                SetBkColor(
                    controlDc,
                    GetSysColor(
                        COLOR_BTNFACE));

                return reinterpret_cast<LRESULT>(
                    GetSysColorBrush(
                        COLOR_BTNFACE));
            }
        }
        break;

    case WM_CTLCOLORSTATIC:
        {
            HDC controlDc =
                reinterpret_cast<HDC>(
                    wordParameter);

            HWND controlWindow =
                reinterpret_cast<HWND>(
                    longParameter);

            // A disabled edit control is reported through WM_CTLCOLORSTATIC.
            if (context != NULL &&
                controlWindow ==
                    context->normalizeTargetEdit)
            {
                SetTextColor(
                    controlDc,
                    GetSysColor(
                        COLOR_GRAYTEXT));

                SetBkColor(
                    controlDc,
                    GetSysColor(
                        COLOR_BTNFACE));

                return reinterpret_cast<LRESULT>(
                    GetSysColorBrush(
                        COLOR_BTNFACE));
            }
        }
        break;

    case WM_CTLCOLORBTN:
        {
            HDC controlDc =
                reinterpret_cast<HDC>(
                    wordParameter);

            HWND controlWindow =
                reinterpret_cast<HWND>(
                    longParameter);

            const int controlId =
                GetDlgCtrlID(
                    controlWindow);

            if (controlId ==
                    MAXGLB_ID_IMPORT_TEXTURES ||
                controlId ==
                    MAXGLB_ID_ADD_PARENT ||
                controlId ==
                    MAXGLB_ID_IMPORT_ANIMATION)
            {
                SetBkMode(
                    controlDc,
                    OPAQUE);

                SetBkColor(
                    controlDc,
                    GetSysColor(
                        COLOR_BTNFACE));

                SetTextColor(
                    controlDc,
                    IsWindowEnabled(controlWindow)
                        ? GetSysColor(
                            COLOR_BTNTEXT)
                        : GetSysColor(
                            COLOR_GRAYTEXT));

                return reinterpret_cast<LRESULT>(
                    GetSysColorBrush(
                        COLOR_BTNFACE));
            }
        }
        break;

    case WM_DRAWITEM:
        {
            const DRAWITEMSTRUCT* drawItem =
                reinterpret_cast<const DRAWITEMSTRUCT*>(
                    longParameter);

            if (drawItem == NULL ||
                drawItem->CtlType != ODT_BUTTON)
            {
                break;
            }

            const int controlId =
                static_cast<int>(
                    drawItem->CtlID);

            if (IsImportCheckboxId(controlId))
            {
                DrawLightImportCheckbox(drawItem);
                return TRUE;
            }

            if (IsImportPushButtonId(controlId))
            {
                DrawLightImportPushButton(drawItem);
                return TRUE;
            }
        }
        break;

    case WM_COMMAND:
        {
            if (context == NULL)
            {
                break;
            }

            const int controlId =
                LOWORD(wordParameter);

            const int notificationCode =
                HIWORD(wordParameter);

            if ((controlId == MAXGLB_ID_IMPORT_TEXTURES ||
                 controlId == MAXGLB_ID_ADD_PARENT ||
                 controlId == MAXGLB_ID_NORMALIZE_SIZE) &&
                notificationCode == BN_CLICKED)
            {
                HWND checkbox =
                    reinterpret_cast<HWND>(
                        longParameter);

                const BOOL newState =
                    !GetImportCheckboxState(
                        checkbox);

                SetImportCheckboxState(
                    checkbox,
                    newState);

                if (controlId == MAXGLB_ID_NORMALIZE_SIZE &&
                    context->normalizeTargetEdit != NULL)
                {
                    EnableWindow(
                        context->normalizeTargetEdit,
                        newState);

                    InvalidateRect(
                        context->normalizeTargetEdit,
                        NULL,
                        TRUE);
                }

                UpdateWindow(checkbox);
                return 0;
            }

            if (controlId == MAXGLB_ID_PREVIEW_ROTATE_UP &&
                notificationCode == BN_CLICKED)
            {
                ApplyImportQuarterTurn(
                    &context->settings,
                    0,
                    -1);

                InvalidateRect(
                    context->previewWindow,
                    NULL,
                    TRUE);

                UpdateWindow(
                    context->previewWindow);
                return 0;
            }

            if (controlId == MAXGLB_ID_PREVIEW_ROTATE_DOWN &&
                notificationCode == BN_CLICKED)
            {
                ApplyImportQuarterTurn(
                    &context->settings,
                    0,
                    1);

                InvalidateRect(
                    context->previewWindow,
                    NULL,
                    TRUE);

                UpdateWindow(
                    context->previewWindow);
                return 0;
            }

            if (controlId == MAXGLB_ID_PREVIEW_ROTATE_LEFT &&
                notificationCode == BN_CLICKED)
            {
                ApplyImportQuarterTurn(
                    &context->settings,
                    2,
                    1);

                InvalidateRect(
                    context->previewWindow,
                    NULL,
                    TRUE);

                UpdateWindow(
                    context->previewWindow);
                return 0;
            }

            if (controlId == MAXGLB_ID_PREVIEW_ROTATE_RESET &&
                notificationCode == BN_CLICKED)
            {
                ResetImportRotationMatrix(
                    &context->settings);

                InvalidateRect(
                    context->previewWindow,
                    NULL,
                    TRUE);

                UpdateWindow(
                    context->previewWindow);
                return 0;
            }

            if (controlId == MAXGLB_ID_PREVIEW_ROTATE_RIGHT &&
                notificationCode == BN_CLICKED)
            {
                ApplyImportQuarterTurn(
                    &context->settings,
                    2,
                    -1);

                InvalidateRect(
                    context->previewWindow,
                    NULL,
                    TRUE);

                UpdateWindow(
                    context->previewWindow);
                return 0;
            }

            if (controlId == MAXGLB_ID_INFO)
            {
                MessageBox(
                    window,
                    _T("MaxGLB2016 imports static triangle meshes for editing and optimization in 3ds Max 2016.\n\n")
                    _T("The preview uses the 3ds Max Front view: X is horizontal, Z+ points upward, and the camera looks along -Y.\n\n")
                    _T("The four arrow buttons rotate both the preview and the final imported model in exact 90-degree steps. Up/down rotate around Max X; left/right rotate around Max Z. The center 0 button restores the standard orientation.\n\n")
                    _T("When Add parent node is enabled, all created mesh nodes are attached below a Dummy helper named Root:NAME.\n\n")
                    _T("Normalize size scales the complete imported model uniformly so its largest world-space dimension equals the entered Max-unit value.\n\n")
                    _T("Animation is intentionally outside the current static-model workflow."),
                    _T("MaxGLB2016 Import Information"),
                    MB_OK |
                    MB_ICONINFORMATION);
                return 0;
            }

            if (controlId == MAXGLB_ID_CANCEL ||
                controlId == IDCANCEL)
            {
                context->accepted = FALSE;
                DestroyWindow(window);
                return 0;
            }

            if (controlId == MAXGLB_ID_IMPORT ||
                controlId == IDOK)
            {
                context->settings.importTextures =
                    GetImportCheckboxState(
                        context->importTexturesCheck);

                context->settings.addParentNode =
                    context->importableObjectCount > 1 &&
                    GetImportCheckboxState(
                        context->addParentCheck);

                context->settings.normalizeSize =
                    GetImportCheckboxState(
                        context->normalizeSizeCheck);

                if (context->settings.normalizeSize)
                {
                    float targetSize = 1.0f;

                    if (!TryReadPositiveImportSize(
                            context->normalizeTargetEdit,
                            &targetSize))
                    {
                        MessageBox(
                            window,
                            _T("Please enter a positive normalize size."),
                            _T("MaxGLB2016"),
                            MB_OK |
                            MB_ICONWARNING);
                        return 0;
                    }

                    context->settings.normalizeTargetSize =
                        targetSize;
                }

                context->settings.appliedUniformScale =
                    1.0f;

                context->settings.importAnimation =
                    FALSE;

                context->accepted = TRUE;
                DestroyWindow(window);
                return 0;
            }
        }
        break;

    case WM_CLOSE:
        if (context != NULL)
        {
            context->accepted = FALSE;
        }

        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        if (context != NULL)
        {
            context->closed = TRUE;
            context->window = NULL;
        }
        return 0;
    }

    return DefWindowProc(
        window,
        message,
        wordParameter,
        longParameter);
}


static BOOL RegisterMaxGLBImportWindowClasses()
{
    WNDCLASSEX previewClass;
    ZeroMemory(
        &previewClass,
        sizeof(previewClass));

    previewClass.cbSize =
        sizeof(previewClass);

    previewClass.style =
        CS_HREDRAW |
        CS_VREDRAW;

    previewClass.lpfnWndProc =
        MaxGLBPreviewWindowProcedure;

    previewClass.hInstance =
        hInstance;

    previewClass.hCursor =
        LoadCursor(
            NULL,
            IDC_ARROW);

    previewClass.lpszClassName =
        MAXGLB_PREVIEW_WINDOW_CLASS;

    if (!RegisterClassEx(&previewClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return FALSE;
    }

    WNDCLASSEX importClass;
    ZeroMemory(
        &importClass,
        sizeof(importClass));

    importClass.cbSize =
        sizeof(importClass);

    importClass.style =
        CS_HREDRAW |
        CS_VREDRAW;

    importClass.lpfnWndProc =
        MaxGLBImportWindowProcedure;

    importClass.hInstance =
        hInstance;

    importClass.hIcon =
        LoadIcon(
            NULL,
            IDI_APPLICATION);

    importClass.hCursor =
        LoadCursor(
            NULL,
            IDC_ARROW);

    importClass.hbrBackground =
        reinterpret_cast<HBRUSH>(
            COLOR_BTNFACE + 1);

    importClass.lpszClassName =
        MAXGLB_IMPORT_WINDOW_CLASS;

    if (!RegisterClassEx(&importClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return FALSE;
    }

    return TRUE;
}


static BOOL ShowMaxGLBImportOptions(
    HWND parentWindow,
    const TCHAR* filename,
    const cgltf_data* data,
    MaxGLBImportSettings* settings)
{
    if (data == NULL ||
        settings == NULL ||
        !RegisterMaxGLBImportWindowClasses())
    {
        return FALSE;
    }

    MaxGLBImportDialogContext context;
    context.parentWindow =
        parentWindow;

    context.filename =
        filename;

    context.data =
        data;

    context.settings =
        *settings;

    context.importableObjectCount =
        CountImportableSceneMeshNodes(data);

    CollectScenePreviewPoints(
        data,
        context.previewPoints);

    const int clientWidth = 700;
    const int clientHeight = 460;

    RECT windowRect =
    {
        0,
        0,
        clientWidth,
        clientHeight
    };

    AdjustWindowRectEx(
        &windowRect,
        WS_CAPTION |
        WS_SYSMENU |
        WS_POPUP,
        FALSE,
        WS_EX_DLGMODALFRAME |
        WS_EX_CONTROLPARENT);

    const int windowWidth =
        windowRect.right -
        windowRect.left;

    const int windowHeight =
        windowRect.bottom -
        windowRect.top;

    int windowX =
        CW_USEDEFAULT;

    int windowY =
        CW_USEDEFAULT;

    if (parentWindow != NULL)
    {
        RECT parentRect;

        if (GetWindowRect(
                parentWindow,
                &parentRect))
        {
            windowX =
                parentRect.left +
                ((parentRect.right - parentRect.left) -
                 windowWidth) / 2;

            windowY =
                parentRect.top +
                ((parentRect.bottom - parentRect.top) -
                 windowHeight) / 2;
        }
    }

    if (parentWindow != NULL)
    {
        EnableWindow(
            parentWindow,
            FALSE);
    }

    HWND importWindow =
        CreateWindowEx(
            WS_EX_DLGMODALFRAME |
            WS_EX_CONTROLPARENT,
            MAXGLB_IMPORT_WINDOW_CLASS,
            _T("MaxGLB2016 Import Options"),
            WS_CAPTION |
            WS_SYSMENU |
            WS_POPUP,
            windowX,
            windowY,
            windowWidth,
            windowHeight,
            parentWindow,
            NULL,
            hInstance,
            &context);

    if (importWindow == NULL)
    {
        if (parentWindow != NULL)
        {
            EnableWindow(
                parentWindow,
                TRUE);
        }

        return FALSE;
    }

    ShowWindow(
        importWindow,
        SW_SHOW);

    UpdateWindow(
        importWindow);

    MSG message;
    BOOL receivedQuitMessage = FALSE;
    int quitCode = 0;

    while (!context.closed)
    {
        const BOOL messageResult =
            GetMessage(
                &message,
                NULL,
                0,
                0);

        if (messageResult <= 0)
        {
            if (messageResult == 0)
            {
                receivedQuitMessage = TRUE;
                quitCode =
                    static_cast<int>(
                        message.wParam);
            }

            break;
        }

        if (!IsDialogMessage(
                importWindow,
                &message))
        {
            TranslateMessage(
                &message);

            DispatchMessage(
                &message);
        }
    }

    if (IsWindow(importWindow))
    {
        DestroyWindow(importWindow);
    }

    if (parentWindow != NULL)
    {
        EnableWindow(
            parentWindow,
            TRUE);

        SetForegroundWindow(
            parentWindow);
    }

    if (receivedQuitMessage)
    {
        PostQuitMessage(
            quitCode);
    }

    if (context.accepted)
    {
        *settings =
            context.settings;
    }

    return context.accepted;
}



static const TCHAR* MAXGLB_EXPORT_WINDOW_CLASS =
    _T("MaxGLB2016ExportOptionsWindow");


struct MaxGLBExportDialogContext
{
    HWND parentWindow;
    HWND window;

    HWND scaleEdit;
    HWND normalizeCheck;
    HWND normalizeTargetEdit;
    HWND materialsCheck;
    HWND texturesCheck;
    HWND alphaModeCombo;
    HWND alphaCutoffEdit;
    HWND summaryCheck;
    HWND explorerCheck;
    HWND animationCheck;

    const TCHAR* outputFilename;
    BOOL selectedOnly;

    MaxGLBExportSettings settings;

    BOOL accepted;
    BOOL closed;

    MaxGLBExportDialogContext()
        : parentWindow(NULL)
        , window(NULL)
        , scaleEdit(NULL)
        , normalizeCheck(NULL)
        , normalizeTargetEdit(NULL)
        , materialsCheck(NULL)
        , texturesCheck(NULL)
        , alphaModeCombo(NULL)
        , alphaCutoffEdit(NULL)
        , summaryCheck(NULL)
        , explorerCheck(NULL)
        , animationCheck(NULL)
        , outputFilename(NULL)
        , selectedOnly(FALSE)
        , accepted(FALSE)
        , closed(FALSE)
    {
    }
};


static BOOL IsExportCheckboxId(
    int controlId)
{
    return controlId == MAXGLB_ID_EXPORT_NORMALIZE ||
        controlId == MAXGLB_ID_EXPORT_MATERIALS ||
        controlId == MAXGLB_ID_EXPORT_TEXTURES ||
        controlId == MAXGLB_ID_EXPORT_SUMMARY ||
        controlId == MAXGLB_ID_EXPORT_EXPLORER ||
        controlId == MAXGLB_ID_EXPORT_ANIMATION;
}


static BOOL IsExportPushButtonId(
    int controlId)
{
    return controlId == MAXGLB_ID_EXPORT_INFO ||
        controlId == MAXGLB_ID_EXPORT_CANCEL ||
        controlId == MAXGLB_ID_EXPORT_CONFIRM;
}


static void UpdateExportDialogEnabledState(
    MaxGLBExportDialogContext* context)
{
    if (context == NULL)
    {
        return;
    }

    const BOOL normalizeEnabled =
        GetImportCheckboxState(
            context->normalizeCheck);

    EnableWindow(
        context->scaleEdit,
        !normalizeEnabled);

    EnableWindow(
        context->normalizeTargetEdit,
        normalizeEnabled);

    const BOOL materialsEnabled =
        GetImportCheckboxState(
            context->materialsCheck);

    EnableWindow(
        context->texturesCheck,
        materialsEnabled);

    EnableWindow(
        context->alphaModeCombo,
        materialsEnabled);

    const int selectedAlphaMode =
        context->alphaModeCombo != NULL
        ? static_cast<int>(
            SendMessage(
                context->alphaModeCombo,
                CB_GETCURSEL,
                0,
                0))
        : MAXGLB_ALPHA_AUTO;

    EnableWindow(
        context->alphaCutoffEdit,
        materialsEnabled &&
        selectedAlphaMode ==
            MAXGLB_ALPHA_MASK);

    InvalidateRect(
        context->scaleEdit,
        NULL,
        TRUE);

    InvalidateRect(
        context->normalizeTargetEdit,
        NULL,
        TRUE);

    InvalidateRect(
        context->texturesCheck,
        NULL,
        TRUE);

    InvalidateRect(
        context->alphaCutoffEdit,
        NULL,
        TRUE);
}


static LRESULT CALLBACK MaxGLBExportWindowProcedure(
    HWND window,
    UINT message,
    WPARAM wordParameter,
    LPARAM longParameter)
{
    MaxGLBExportDialogContext* context =
        reinterpret_cast<MaxGLBExportDialogContext*>(
            GetWindowLongPtr(
                window,
                GWLP_USERDATA));

    switch (message)
    {
    case WM_NCCREATE:
        {
            const CREATESTRUCT* createStruct =
                reinterpret_cast<const CREATESTRUCT*>(
                    longParameter);

            context =
                reinterpret_cast<MaxGLBExportDialogContext*>(
                    createStruct->lpCreateParams);

            SetWindowLongPtr(
                window,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(
                    context));

            if (context != NULL)
            {
                context->window =
                    window;
            }
        }
        break;

    case WM_CREATE:
        {
            context =
                reinterpret_cast<MaxGLBExportDialogContext*>(
                    reinterpret_cast<CREATESTRUCT*>(
                        longParameter)->lpCreateParams);

            if (context == NULL)
            {
                return -1;
            }

            TCHAR modeText[128];

            _stprintf_s(
                modeText,
                _countof(modeText),
                _T("Mode: %s"),
                context->selectedOnly
                    ? _T("Export Selected")
                    : _T("Export Scene"));

            CreateImportControl(
                0,
                _T("STATIC"),
                modeText,
                WS_CHILD |
                WS_VISIBLE,
                16,
                14,
                500,
                20,
                window,
                -1,
                NULL);

            TCHAR outputText[512];

            _stprintf_s(
                outputText,
                _countof(outputText),
                _T("Output: %s"),
                context->outputFilename != NULL
                    ? context->outputFilename
                    : _T(""));

            CreateImportControl(
                0,
                _T("STATIC"),
                outputText,
                WS_CHILD |
                WS_VISIBLE |
                SS_PATHELLIPSIS,
                16,
                36,
                500,
                20,
                window,
                -1,
                NULL);

            CreateImportControl(
                0,
                _T("STATIC"),
                _T("Size"),
                WS_CHILD |
                WS_VISIBLE,
                16,
                72,
                180,
                20,
                window,
                -1,
                NULL);

            CreateImportControl(
                0,
                _T("STATIC"),
                _T("Uniform scale factor:"),
                WS_CHILD |
                WS_VISIBLE,
                30,
                101,
                165,
                20,
                window,
                -1,
                NULL);

            TCHAR scaleText[64];

            _stprintf_s(
                scaleText,
                _countof(scaleText),
                _T("%.6g"),
                context->settings.scaleFactor);

            context->scaleEdit =
                CreateImportControl(
                    WS_EX_CLIENTEDGE,
                    _T("EDIT"),
                    scaleText,
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    ES_AUTOHSCROLL,
                    200,
                    98,
                    95,
                    24,
                    window,
                    MAXGLB_ID_EXPORT_SCALE,
                    NULL);

            UseReadableClassicTheme(
                context->scaleEdit);

            context->normalizeCheck =
                CreateImportControl(
                    0,
                    _T("BUTTON"),
                    _T("Normalize largest dimension to"),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    BS_OWNERDRAW,
                    27,
                    132,
                    245,
                    26,
                    window,
                    MAXGLB_ID_EXPORT_NORMALIZE,
                    NULL);

            SetImportCheckboxState(
                context->normalizeCheck,
                context->settings.normalizeSize);

            TCHAR normalizeText[64];

            _stprintf_s(
                normalizeText,
                _countof(normalizeText),
                _T("%.6g"),
                context->settings.normalizeTargetSize);

            context->normalizeTargetEdit =
                CreateImportControl(
                    WS_EX_CLIENTEDGE,
                    _T("EDIT"),
                    normalizeText,
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    ES_AUTOHSCROLL,
                    275,
                    133,
                    95,
                    24,
                    window,
                    MAXGLB_ID_EXPORT_NORMALIZE_TARGET,
                    NULL);

            UseReadableClassicTheme(
                context->normalizeTargetEdit);

            CreateImportControl(
                0,
                _T("STATIC"),
                _T("Max units"),
                WS_CHILD |
                WS_VISIBLE,
                378,
                137,
                90,
                20,
                window,
                -1,
                NULL);

            CreateImportControl(
                0,
                _T("STATIC"),
                _T("Materials"),
                WS_CHILD |
                WS_VISIBLE,
                16,
                176,
                180,
                20,
                window,
                -1,
                NULL);

            context->materialsCheck =
                CreateImportControl(
                    0,
                    _T("BUTTON"),
                    _T("Export Standard materials"),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    BS_OWNERDRAW,
                    27,
                    204,
                    250,
                    26,
                    window,
                    MAXGLB_ID_EXPORT_MATERIALS,
                    NULL);

            SetImportCheckboxState(
                context->materialsCheck,
                context->settings.exportMaterials);

            context->texturesCheck =
                CreateImportControl(
                    0,
                    _T("BUTTON"),
                    _T("Export textures (embedded in GLB)"),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    BS_OWNERDRAW,
                    27,
                    234,
                    300,
                    26,
                    window,
                    MAXGLB_ID_EXPORT_TEXTURES,
                    NULL);

            SetImportCheckboxState(
                context->texturesCheck,
                context->settings.exportTextures);

            CreateImportControl(
                0,
                _T("STATIC"),
                _T("Alpha mode"),
                WS_CHILD |
                WS_VISIBLE,
                315,
                176,
                190,
                20,
                window,
                -1,
                NULL);

            context->alphaModeCombo =
                CreateImportControl(
                    WS_EX_CLIENTEDGE,
                    _T("COMBOBOX"),
                    _T(""),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    CBS_DROPDOWNLIST |
                    WS_VSCROLL,
                    315,
                    202,
                    200,
                    120,
                    window,
                    MAXGLB_ID_EXPORT_ALPHA_MODE,
                    NULL);

            SendMessage(
                context->alphaModeCombo,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(
                    _T("Auto / preserve")));

            SendMessage(
                context->alphaModeCombo,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(
                    _T("OPAQUE")));

            SendMessage(
                context->alphaModeCombo,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(
                    _T("MASK")));

            SendMessage(
                context->alphaModeCombo,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(
                    _T("BLEND")));

            SendMessage(
                context->alphaModeCombo,
                CB_SETCURSEL,
                context->settings.alphaMode,
                0);

            CreateImportControl(
                0,
                _T("STATIC"),
                _T("Mask cutoff"),
                WS_CHILD |
                WS_VISIBLE,
                315,
                234,
                90,
                20,
                window,
                -1,
                NULL);

            TCHAR alphaCutoffText[64];

            _stprintf_s(
                alphaCutoffText,
                _countof(alphaCutoffText),
                _T("%.4g"),
                context->settings.alphaCutoff);

            context->alphaCutoffEdit =
                CreateImportControl(
                    WS_EX_CLIENTEDGE,
                    _T("EDIT"),
                    alphaCutoffText,
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    ES_AUTOHSCROLL,
                    410,
                    231,
                    80,
                    24,
                    window,
                    MAXGLB_ID_EXPORT_ALPHA_CUTOFF,
                    NULL);

            UseReadableClassicTheme(
                context->alphaCutoffEdit);

            CreateImportControl(
                0,
                _T("STATIC"),
                _T("Geometry is always triangulated; modifier stacks, transforms, normals and UV channel 1 are exported automatically."),
                WS_CHILD |
                WS_VISIBLE,
                28,
                266,
                470,
                36,
                window,
                -1,
                NULL);

            CreateImportControl(
                0,
                _T("STATIC"),
                _T("After export"),
                WS_CHILD |
                WS_VISIBLE,
                16,
                312,
                180,
                20,
                window,
                -1,
                NULL);

            context->summaryCheck =
                CreateImportControl(
                    0,
                    _T("BUTTON"),
                    _T("Show export summary"),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    BS_OWNERDRAW,
                    27,
                    338,
                    230,
                    26,
                    window,
                    MAXGLB_ID_EXPORT_SUMMARY,
                    NULL);

            SetImportCheckboxState(
                context->summaryCheck,
                context->settings.showSummary);

            context->explorerCheck =
                CreateImportControl(
                    0,
                    _T("BUTTON"),
                    _T("Show exported file in Windows Explorer"),
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_TABSTOP |
                    BS_OWNERDRAW,
                    27,
                    368,
                    330,
                    26,
                    window,
                    MAXGLB_ID_EXPORT_EXPLORER,
                    NULL);

            SetImportCheckboxState(
                context->explorerCheck,
                context->settings.showInExplorer);

            context->animationCheck =
                CreateImportControl(
                    0,
                    _T("BUTTON"),
                    _T("Export animation (not implemented)"),
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_OWNERDRAW,
                    27,
                    398,
                    300,
                    26,
                    window,
                    MAXGLB_ID_EXPORT_ANIMATION,
                    NULL);

            SetImportCheckboxState(
                context->animationCheck,
                FALSE);

            EnableWindow(
                context->animationCheck,
                FALSE);

            CreateImportControl(
                0,
                _T("BUTTON"),
                _T("Info"),
                WS_CHILD |
                WS_VISIBLE |
                WS_TABSTOP |
                BS_OWNERDRAW,
                16,
                442,
                80,
                29,
                window,
                MAXGLB_ID_EXPORT_INFO,
                NULL);

            CreateImportControl(
                0,
                _T("BUTTON"),
                _T("Cancel"),
                WS_CHILD |
                WS_VISIBLE |
                WS_TABSTOP |
                BS_OWNERDRAW,
                350,
                442,
                80,
                29,
                window,
                MAXGLB_ID_EXPORT_CANCEL,
                NULL);

            CreateImportControl(
                0,
                _T("BUTTON"),
                _T("Export"),
                WS_CHILD |
                WS_VISIBLE |
                WS_TABSTOP |
                BS_OWNERDRAW,
                440,
                442,
                80,
                29,
                window,
                MAXGLB_ID_EXPORT_CONFIRM,
                NULL);

            SendMessage(
                window,
                DM_SETDEFID,
                MAXGLB_ID_EXPORT_CONFIRM,
                0);

            UpdateExportDialogEnabledState(
                context);
        }
        return 0;

    case WM_CTLCOLOREDIT:
        {
            HDC controlDc =
                reinterpret_cast<HDC>(
                    wordParameter);

            HWND controlWindow =
                reinterpret_cast<HWND>(
                    longParameter);

            if (context != NULL &&
                (controlWindow == context->scaleEdit ||
                 controlWindow == context->normalizeTargetEdit ||
                 controlWindow == context->alphaCutoffEdit))
            {
                if (IsWindowEnabled(controlWindow))
                {
                    SetTextColor(
                        controlDc,
                        GetSysColor(COLOR_WINDOWTEXT));

                    SetBkColor(
                        controlDc,
                        GetSysColor(COLOR_WINDOW));

                    return reinterpret_cast<LRESULT>(
                        GetSysColorBrush(COLOR_WINDOW));
                }

                SetTextColor(
                    controlDc,
                    GetSysColor(COLOR_GRAYTEXT));

                SetBkColor(
                    controlDc,
                    GetSysColor(COLOR_BTNFACE));

                return reinterpret_cast<LRESULT>(
                    GetSysColorBrush(COLOR_BTNFACE));
            }
        }
        break;

    case WM_CTLCOLORSTATIC:
        {
            HDC controlDc =
                reinterpret_cast<HDC>(
                    wordParameter);

            HWND controlWindow =
                reinterpret_cast<HWND>(
                    longParameter);

            if (context != NULL &&
                (controlWindow == context->scaleEdit ||
                 controlWindow == context->normalizeTargetEdit ||
                 controlWindow == context->alphaCutoffEdit))
            {
                SetTextColor(
                    controlDc,
                    GetSysColor(COLOR_GRAYTEXT));

                SetBkColor(
                    controlDc,
                    GetSysColor(COLOR_BTNFACE));

                return reinterpret_cast<LRESULT>(
                    GetSysColorBrush(COLOR_BTNFACE));
            }
        }
        break;

    case WM_DRAWITEM:
        {
            const DRAWITEMSTRUCT* drawItem =
                reinterpret_cast<const DRAWITEMSTRUCT*>(
                    longParameter);

            if (drawItem == NULL ||
                drawItem->CtlType != ODT_BUTTON)
            {
                break;
            }

            const int controlId =
                static_cast<int>(
                    drawItem->CtlID);

            if (IsExportCheckboxId(controlId))
            {
                DrawLightImportCheckbox(drawItem);
                return TRUE;
            }

            if (IsExportPushButtonId(controlId))
            {
                DrawLightImportPushButton(drawItem);
                return TRUE;
            }
        }
        break;

    case WM_COMMAND:
        {
            if (context == NULL)
            {
                break;
            }

            const int controlId =
                LOWORD(wordParameter);

            const int notificationCode =
                HIWORD(wordParameter);

            if ((controlId == MAXGLB_ID_EXPORT_NORMALIZE ||
                 controlId == MAXGLB_ID_EXPORT_MATERIALS ||
                 controlId == MAXGLB_ID_EXPORT_TEXTURES ||
                 controlId == MAXGLB_ID_EXPORT_SUMMARY ||
                 controlId == MAXGLB_ID_EXPORT_EXPLORER) &&
                notificationCode == BN_CLICKED)
            {
                HWND checkbox =
                    reinterpret_cast<HWND>(
                        longParameter);

                if (IsWindowEnabled(checkbox))
                {
                    SetImportCheckboxState(
                        checkbox,
                        !GetImportCheckboxState(
                            checkbox));
                }

                UpdateExportDialogEnabledState(
                    context);

                UpdateWindow(checkbox);
                return 0;
            }

            if (controlId == MAXGLB_ID_EXPORT_ALPHA_MODE &&
                notificationCode == CBN_SELCHANGE)
            {
                UpdateExportDialogEnabledState(
                    context);
                return 0;
            }

            if (controlId == MAXGLB_ID_EXPORT_INFO)
            {
                MessageBox(
                    window,
                    _T("MaxGLB2016 exports static models for editing, optimization and arrangement workflows.\n\n")
                    _T("Uniform scale multiplies all baked world-space positions. Normalize instead scales the complete export uniformly so its largest dimension matches the requested Max-unit value.\n\n")
                    _T("GLB stores all enabled textures inside the output file. Standard material colors and opacity can still be exported when texture export is disabled.\n\n")
                    _T("Alpha Auto preserves OPAQUE, BLEND, MASK and alphaCutoff from imported GLB materials. For manually created Max materials it chooses BLEND when opacity is present, otherwise OPAQUE. MASK can be forced with a custom cutoff.\n\n")
                    _T("Animation is intentionally outside the current static-model workflow."),
                    _T("MaxGLB2016 Export Information"),
                    MB_OK |
                    MB_ICONINFORMATION);
                return 0;
            }

            if (controlId == MAXGLB_ID_EXPORT_CANCEL ||
                controlId == IDCANCEL)
            {
                context->accepted = FALSE;
                DestroyWindow(window);
                return 0;
            }

            if (controlId == MAXGLB_ID_EXPORT_CONFIRM ||
                controlId == IDOK)
            {
                float scaleFactor = 1.0f;
                float normalizeTarget = 1.0f;
                float alphaCutoff = 0.5f;

                const BOOL normalizeSize =
                    GetImportCheckboxState(
                        context->normalizeCheck);

                if (!normalizeSize &&
                    !TryReadPositiveImportSize(
                        context->scaleEdit,
                        &scaleFactor))
                {
                    MessageBox(
                        window,
                        _T("Please enter a positive uniform scale factor."),
                        _T("MaxGLB2016"),
                        MB_OK |
                        MB_ICONWARNING);
                    return 0;
                }

                if (normalizeSize &&
                    !TryReadPositiveImportSize(
                        context->normalizeTargetEdit,
                        &normalizeTarget))
                {
                    MessageBox(
                        window,
                        _T("Please enter a positive normalize size."),
                        _T("MaxGLB2016"),
                        MB_OK |
                        MB_ICONWARNING);
                    return 0;
                }

                const int selectedAlphaMode =
                    static_cast<int>(
                        SendMessage(
                            context->alphaModeCombo,
                            CB_GETCURSEL,
                            0,
                            0));

                if (selectedAlphaMode ==
                        MAXGLB_ALPHA_MASK &&
                    !TryReadUnitFloat(
                        context->alphaCutoffEdit,
                        &alphaCutoff))
                {
                    MessageBox(
                        window,
                        _T("Alpha cutoff must be a number between 0 and 1."),
                        _T("MaxGLB2016"),
                        MB_OK |
                        MB_ICONWARNING);
                    return 0;
                }

                context->settings.scaleFactor =
                    scaleFactor;

                context->settings.normalizeSize =
                    normalizeSize;

                context->settings.normalizeTargetSize =
                    normalizeTarget;

                context->settings.appliedUniformScale =
                    1.0f;

                context->settings.exportMaterials =
                    GetImportCheckboxState(
                        context->materialsCheck);

                context->settings.alphaMode =
                    context->settings.exportMaterials
                    ? selectedAlphaMode
                    : MAXGLB_ALPHA_OPAQUE;

                context->settings.alphaCutoff =
                    ClampMaxGLBAlphaCutoff(
                        alphaCutoff);

                context->settings.exportTextures =
                    context->settings.exportMaterials &&
                    GetImportCheckboxState(
                        context->texturesCheck);

                context->settings.showSummary =
                    GetImportCheckboxState(
                        context->summaryCheck);

                context->settings.showInExplorer =
                    GetImportCheckboxState(
                        context->explorerCheck);

                context->settings.exportAnimation =
                    FALSE;

                context->accepted = TRUE;
                DestroyWindow(window);
                return 0;
            }
        }
        break;

    case WM_CLOSE:
        if (context != NULL)
        {
            context->accepted = FALSE;
        }

        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        if (context != NULL)
        {
            context->closed = TRUE;
            context->window = NULL;
        }
        return 0;
    }

    return DefWindowProc(
        window,
        message,
        wordParameter,
        longParameter);
}


static BOOL RegisterMaxGLBExportWindowClass()
{
    WNDCLASSEX exportClass;
    ZeroMemory(
        &exportClass,
        sizeof(exportClass));

    exportClass.cbSize =
        sizeof(exportClass);

    exportClass.style =
        CS_HREDRAW |
        CS_VREDRAW;

    exportClass.lpfnWndProc =
        MaxGLBExportWindowProcedure;

    exportClass.hInstance =
        hInstance;

    exportClass.hIcon =
        LoadIcon(
            NULL,
            IDI_APPLICATION);

    exportClass.hCursor =
        LoadCursor(
            NULL,
            IDC_ARROW);

    exportClass.hbrBackground =
        reinterpret_cast<HBRUSH>(
            COLOR_BTNFACE + 1);

    exportClass.lpszClassName =
        MAXGLB_EXPORT_WINDOW_CLASS;

    if (!RegisterClassEx(&exportClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return FALSE;
    }

    return TRUE;
}


static BOOL ShowMaxGLBExportOptions(
    HWND parentWindow,
    const TCHAR* outputFilename,
    BOOL selectedOnly,
    MaxGLBExportSettings* settings)
{
    if (settings == NULL ||
        !RegisterMaxGLBExportWindowClass())
    {
        return FALSE;
    }

    MaxGLBExportDialogContext context;

    context.parentWindow =
        parentWindow;

    context.outputFilename =
        outputFilename;

    context.selectedOnly =
        selectedOnly;

    context.settings =
        *settings;

    const int clientWidth = 536;
    const int clientHeight = 486;

    RECT windowRect =
    {
        0,
        0,
        clientWidth,
        clientHeight
    };

    AdjustWindowRectEx(
        &windowRect,
        WS_CAPTION |
        WS_SYSMENU |
        WS_POPUP,
        FALSE,
        WS_EX_DLGMODALFRAME |
        WS_EX_CONTROLPARENT);

    const int windowWidth =
        windowRect.right -
        windowRect.left;

    const int windowHeight =
        windowRect.bottom -
        windowRect.top;

    int windowX =
        CW_USEDEFAULT;

    int windowY =
        CW_USEDEFAULT;

    if (parentWindow != NULL)
    {
        RECT parentRect;

        if (GetWindowRect(
                parentWindow,
                &parentRect))
        {
            windowX =
                parentRect.left +
                ((parentRect.right - parentRect.left) -
                 windowWidth) / 2;

            windowY =
                parentRect.top +
                ((parentRect.bottom - parentRect.top) -
                 windowHeight) / 2;
        }
    }

    if (parentWindow != NULL)
    {
        EnableWindow(
            parentWindow,
            FALSE);
    }

    HWND exportWindow =
        CreateWindowEx(
            WS_EX_DLGMODALFRAME |
            WS_EX_CONTROLPARENT,
            MAXGLB_EXPORT_WINDOW_CLASS,
            _T("MaxGLB2016 Export Options"),
            WS_CAPTION |
            WS_SYSMENU |
            WS_POPUP,
            windowX,
            windowY,
            windowWidth,
            windowHeight,
            parentWindow,
            NULL,
            hInstance,
            &context);

    if (exportWindow == NULL)
    {
        if (parentWindow != NULL)
        {
            EnableWindow(
                parentWindow,
                TRUE);
        }

        return FALSE;
    }

    ShowWindow(
        exportWindow,
        SW_SHOW);

    UpdateWindow(
        exportWindow);

    MSG message;
    BOOL receivedQuitMessage = FALSE;
    int quitCode = 0;

    while (!context.closed)
    {
        const BOOL messageResult =
            GetMessage(
                &message,
                NULL,
                0,
                0);

        if (messageResult <= 0)
        {
            if (messageResult == 0)
            {
                receivedQuitMessage = TRUE;
                quitCode =
                    static_cast<int>(
                        message.wParam);
            }

            break;
        }

        if (!IsDialogMessage(
                exportWindow,
                &message))
        {
            TranslateMessage(
                &message);

            DispatchMessage(
                &message);
        }
    }

    if (IsWindow(exportWindow))
    {
        DestroyWindow(exportWindow);
    }

    if (parentWindow != NULL)
    {
        EnableWindow(
            parentWindow,
            TRUE);

        SetForegroundWindow(
            parentWindow);
    }

    if (receivedQuitMessage)
    {
        PostQuitMessage(
            quitCode);
    }

    if (context.accepted)
    {
        *settings =
            context.settings;
    }

    return context.accepted;
}


static void BuildImportedNodeName(
    const cgltf_node* sourceNode,
    const cgltf_mesh* sourceMesh,
    int primitiveIndex,
    int importObjectIndex,
    TCHAR* outputName,
    size_t outputNameCount)
{
    if (outputName == NULL ||
        outputNameCount == 0)
    {
        return;
    }

    TCHAR baseName[192];
    baseName[0] = _T('\0');

    if (sourceNode != NULL)
    {
        ConvertUtf8ToTchar(
            sourceNode->name,
            baseName,
            _countof(baseName));
    }

    if (baseName[0] == _T('\0') &&
        sourceMesh != NULL)
    {
        ConvertUtf8ToTchar(
            sourceMesh->name,
            baseName,
            _countof(baseName));
    }

    if (baseName[0] == _T('\0'))
    {
        _stprintf_s(
            baseName,
            _countof(baseName),
            _T("MaxGLB2016_Mesh_%d"),
            importObjectIndex);
    }

    if (sourceMesh != NULL &&
        sourceMesh->primitives_count > 1)
    {
        _stprintf_s(
            outputName,
            outputNameCount,
            _T("%s_Primitive_%d"),
            baseName,
            primitiveIndex);
    }
    else
    {
        _tcscpy_s(
            outputName,
            outputNameCount,
            baseName);
    }
}


static void BuildImportedMeshNodeName(
    const cgltf_node* sourceNode,
    const cgltf_mesh* sourceMesh,
    int importObjectIndex,
    TCHAR* outputName,
    size_t outputNameCount)
{
    if (outputName == NULL ||
        outputNameCount == 0)
    {
        return;
    }

    outputName[0] =
        _T('\0');

    if (sourceNode != NULL)
    {
        ConvertUtf8ToTchar(
            sourceNode->name,
            outputName,
            outputNameCount);
    }

    if (outputName[0] == _T('\0') &&
        sourceMesh != NULL)
    {
        ConvertUtf8ToTchar(
            sourceMesh->name,
            outputName,
            outputNameCount);
    }

    if (outputName[0] == _T('\0'))
    {
        _stprintf_s(
            outputName,
            outputNameCount,
            _T("MaxGLB2016_Mesh_%d"),
            importObjectIndex);
    }
}


static BOOL ImportPrimitiveAsNode(
    cgltf_data* data,
    const TCHAR* sourceFilename,
    const cgltf_node* sourceNode,
    cgltf_mesh* sourceMesh,
    cgltf_primitive* primitive,
    int primitiveIndex,
    int importObjectIndex,
    Interface* maxInterface,
    struct MaxGLBImportStats* importStats,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (data == NULL ||
        sourceFilename == NULL ||
        sourceNode == NULL ||
        sourceMesh == NULL ||
        primitive == NULL ||
        maxInterface == NULL ||
        importStats == NULL)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The importer received invalid primitive data."));
        return FALSE;
    }

    if (primitive->type != cgltf_primitive_type_triangles)
    {
        return TRUE;
    }

    TCHAR importProgressName[320];

    BuildImportedNodeName(
        sourceNode,
        sourceMesh,
        primitiveIndex,
        importObjectIndex,
        importProgressName,
        _countof(importProgressName));

    TCHAR importProgressStep[384];

    _stprintf_s(
        importProgressStep,
        _countof(importProgressStep),
        _T("Importing %s (%d of %d)"),
        importProgressName,
        importObjectIndex + 1,
        g_activeImportPrimitiveCount);

    const int importProgressPercent =
        5 +
        (importObjectIndex * 88) /
        (g_activeImportPrimitiveCount > 0
            ? g_activeImportPrimitiveCount
            : 1);

    if (!UpdateMaxGLBProgress(
            importProgressPercent,
            importProgressStep))
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Import cancelled by the user."));
        return FALSE;
    }

    const cgltf_accessor* positionAccessor =
        cgltf_find_accessor(
            primitive,
            cgltf_attribute_type_position,
            0);

    if (positionAccessor == NULL ||
        positionAccessor->count == 0)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The triangle primitive contains no vertex positions."));
        return FALSE;
    }

    if (positionAccessor->is_sparse)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Sparse POSITION accessors are not supported yet."));
        return FALSE;
    }

    const cgltf_accessor* texCoordAccessor =
        cgltf_find_accessor(
            primitive,
            cgltf_attribute_type_texcoord,
            0);

    BOOL hasUvChannel = FALSE;

    if (texCoordAccessor != NULL)
    {
        if (texCoordAccessor->type != cgltf_type_vec2)
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("TEXCOORD_0 is present but is not a two-component accessor."));
            return FALSE;
        }

        if (texCoordAccessor->count != positionAccessor->count)
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("TEXCOORD_0 and POSITION contain different vertex counts."));
            return FALSE;
        }

        if (texCoordAccessor->is_sparse)
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("Sparse TEXCOORD_0 accessors are not supported yet."));
            return FALSE;
        }

        hasUvChannel = TRUE;
    }

    const cgltf_accessor* normalAccessor =
        cgltf_find_accessor(
            primitive,
            cgltf_attribute_type_normal,
            0);

    BOOL hasNormals = FALSE;

    if (normalAccessor != NULL)
    {
        if (normalAccessor->type != cgltf_type_vec3)
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("NORMAL is present but is not a three-component accessor."));
            return FALSE;
        }

        if (normalAccessor->count != positionAccessor->count)
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("NORMAL and POSITION contain different vertex counts."));
            return FALSE;
        }

        if (normalAccessor->is_sparse)
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("Sparse NORMAL accessors are not supported yet."));
            return FALSE;
        }

        hasNormals = TRUE;
    }

    cgltf_size indexCount =
        primitive->indices != NULL
        ? primitive->indices->count
        : positionAccessor->count;

    if (indexCount < 3 || (indexCount % 3) != 0)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The primitive index count is not a valid triangle list."));
        return FALSE;
    }

    if (primitive->indices != NULL &&
        primitive->indices->is_sparse)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Sparse index accessors are not supported yet."));
        return FALSE;
    }

    cgltf_size faceCount = indexCount / 3;

    if (positionAccessor->count > static_cast<cgltf_size>(INT_MAX) ||
        faceCount > static_cast<cgltf_size>(INT_MAX))
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The mesh is too large for this 3ds Max importer."));
        return FALSE;
    }

    cgltf_float gltfWorld[16] =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    if (sourceNode != NULL)
    {
        BuildGltfWorldMatrix(
            data,
            sourceNode,
            gltfWorld);
    }

    // glTF often duplicates POSITION entries at UV seams and hard-normal
    // boundaries. Max can share the geometric vertex while keeping UV
    // vertices and explicit normals separate per face corner.
    std::vector<Point3> weldedPositions;
    std::vector<int> sourceToWelded(
        static_cast<size_t>(positionAccessor->count));

    std::map<MaxGLBPositionKey, int> weldedLookup;

    weldedPositions.reserve(
        static_cast<size_t>(positionAccessor->count));

    for (cgltf_size sourceVertexIndex = 0;
         sourceVertexIndex < positionAccessor->count;
         ++sourceVertexIndex)
    {
        if ((sourceVertexIndex & 4095) == 0 &&
            !ContinueMaxGLBOperation(
                errorMessage,
                errorMessageCount))
        {
            return FALSE;
        }

        cgltf_float localPosition[3];

        if (!cgltf_accessor_read_float(
                positionAccessor,
                sourceVertexIndex,
                localPosition,
                3))
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("A vertex position could not be read."));
            return FALSE;
        }

        cgltf_float worldPosition[3];

        TransformGltfPoint(
            gltfWorld,
            localPosition[0],
            localPosition[1],
            localPosition[2],
            worldPosition);

        Point3 maxPosition =
            ConvertGltfLocalPositionToMax(
                worldPosition);

        MaxGLBPositionKey key(maxPosition);

        std::map<MaxGLBPositionKey, int>::iterator existing =
            weldedLookup.find(key);

        int weldedIndex;

        if (existing != weldedLookup.end())
        {
            weldedIndex = existing->second;
        }
        else
        {
            if (weldedPositions.size() >=
                static_cast<size_t>(INT_MAX))
            {
                _tcscpy_s(
                    errorMessage,
                    errorMessageCount,
                    _T("The welded mesh contains too many vertices."));
                return FALSE;
            }

            weldedIndex =
                static_cast<int>(weldedPositions.size());

            weldedPositions.push_back(maxPosition);
            weldedLookup.insert(
                std::make_pair(key, weldedIndex));
        }

        sourceToWelded[
            static_cast<size_t>(sourceVertexIndex)] =
            weldedIndex;
    }

    TriObject* triObject = CreateNewTriObject();

    if (triObject == NULL)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("3ds Max could not create a TriObject."));
        return FALSE;
    }

    Mesh& maxMesh = triObject->GetMesh();

    maxMesh.setNumVerts(
        static_cast<int>(weldedPositions.size()));

    maxMesh.setNumFaces(
        static_cast<int>(faceCount));

    for (size_t weldedIndex = 0;
         weldedIndex < weldedPositions.size();
         ++weldedIndex)
    {
        maxMesh.setVert(
            static_cast<int>(weldedIndex),
            weldedPositions[weldedIndex]);
    }

    if (hasUvChannel)
    {
        maxMesh.setNumTVerts(
            static_cast<int>(texCoordAccessor->count));

        maxMesh.setNumTVFaces(
            static_cast<int>(faceCount));
    }

    MeshNormalSpec* specifiedNormals = NULL;

    if (hasNormals)
    {
        maxMesh.SpecifyNormals();
        specifiedNormals = maxMesh.GetSpecifiedNormals();

        if (specifiedNormals == NULL)
        {
            triObject->DeleteThis();

            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("3ds Max could not allocate specified normals."));
            return FALSE;
        }

        specifiedNormals->SetParent(&maxMesh);

        if (!specifiedNormals->SetNumFaces(
                static_cast<int>(faceCount)) ||
            !specifiedNormals->SetNumNormals(
                static_cast<int>(normalAccessor->count)))
        {
            triObject->DeleteThis();

            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("3ds Max could not allocate the imported normal data."));
            return FALSE;
        }
    }

    for (cgltf_size sourceVertexIndex = 0;
         sourceVertexIndex < positionAccessor->count;
         ++sourceVertexIndex)
    {
        if ((sourceVertexIndex & 4095) == 0 &&
            !ContinueMaxGLBOperation(
                errorMessage,
                errorMessageCount))
        {
            triObject->DeleteThis();
            return FALSE;
        }

        if (hasUvChannel)
        {
            cgltf_float texCoord[2];

            if (!cgltf_accessor_read_float(
                    texCoordAccessor,
                    sourceVertexIndex,
                    texCoord,
                    2))
            {
                triObject->DeleteThis();

                _tcscpy_s(
                    errorMessage,
                    errorMessageCount,
                    _T("A TEXCOORD_0 value could not be read."));
                return FALSE;
            }

            // glTF and 3ds Max use opposite V directions for bitmap UVs.
            maxMesh.tVerts[
                static_cast<int>(sourceVertexIndex)] =
                UVVert(
                    static_cast<float>(texCoord[0]),
                    static_cast<float>(1.0f - texCoord[1]),
                    0.0f);
        }

        if (hasNormals)
        {
            cgltf_float sourceNormal[3];

            if (!cgltf_accessor_read_float(
                    normalAccessor,
                    sourceVertexIndex,
                    sourceNormal,
                    3))
            {
                triObject->DeleteThis();

                _tcscpy_s(
                    errorMessage,
                    errorMessageCount,
                    _T("A NORMAL value could not be read."));
                return FALSE;
            }

            specifiedNormals->Normal(
                static_cast<int>(sourceVertexIndex)) =
                TransformGltfNormalToMaxWorld(
                    gltfWorld,
                    sourceNormal);

            specifiedNormals->SetNormalExplicit(
                static_cast<int>(sourceVertexIndex),
                true);
        }
    }

    for (cgltf_size faceIndex = 0;
         faceIndex < faceCount;
         ++faceIndex)
    {
        if ((faceIndex & 4095) == 0 &&
            !ContinueMaxGLBOperation(
                errorMessage,
                errorMessageCount))
        {
            triObject->DeleteThis();
            return FALSE;
        }

        cgltf_size sourceIndex0;
        cgltf_size sourceIndex1;
        cgltf_size sourceIndex2;

        if (primitive->indices != NULL)
        {
            sourceIndex0 = cgltf_accessor_read_index(
                primitive->indices,
                faceIndex * 3 + 0);

            sourceIndex1 = cgltf_accessor_read_index(
                primitive->indices,
                faceIndex * 3 + 1);

            sourceIndex2 = cgltf_accessor_read_index(
                primitive->indices,
                faceIndex * 3 + 2);
        }
        else
        {
            sourceIndex0 = faceIndex * 3 + 0;
            sourceIndex1 = faceIndex * 3 + 1;
            sourceIndex2 = faceIndex * 3 + 2;
        }

        if (sourceIndex0 >= positionAccessor->count ||
            sourceIndex1 >= positionAccessor->count ||
            sourceIndex2 >= positionAccessor->count)
        {
            triObject->DeleteThis();

            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("The primitive contains an invalid vertex index."));
            return FALSE;
        }

        const int maxIndex0 =
            sourceToWelded[
                static_cast<size_t>(sourceIndex0)];

        const int maxIndex1 =
            sourceToWelded[
                static_cast<size_t>(sourceIndex1)];

        const int maxIndex2 =
            sourceToWelded[
                static_cast<size_t>(sourceIndex2)];

        Face& face =
            maxMesh.faces[static_cast<int>(faceIndex)];

        face.setVerts(
            maxIndex0,
            maxIndex1,
            maxIndex2);

        face.setEdgeVisFlags(1, 1, 1);
        face.setSmGroup(1);
        face.setMatID(0);

        if (hasUvChannel)
        {
            maxMesh.tvFace[
                static_cast<int>(faceIndex)].setTVerts(
                    static_cast<int>(sourceIndex0),
                    static_cast<int>(sourceIndex1),
                    static_cast<int>(sourceIndex2));
        }

        if (hasNormals)
        {
            MeshNormalFace& normalFace =
                specifiedNormals->Face(
                    static_cast<int>(faceIndex));

            normalFace.SpecifyNormalID(
                0,
                static_cast<int>(sourceIndex0));

            normalFace.SpecifyNormalID(
                1,
                static_cast<int>(sourceIndex1));

            normalFace.SpecifyNormalID(
                2,
                static_cast<int>(sourceIndex2));
        }
    }

    maxMesh.InvalidateGeomCache();
    maxMesh.InvalidateTopologyCache();

    if (hasNormals)
    {
        specifiedNormals->SetAllExplicit(true);
        specifiedNormals->CheckNormals();
    }
    else
    {
        maxMesh.buildNormals();
    }

    Mtl* importedMaterial = NULL;
    BOOL hasBaseColorTexture = FALSE;
    BOOL hasNormalTexture = FALSE;
    BOOL hasOrmChannels = FALSE;
    BOOL hasEmissiveTexture = FALSE;

    g_importTextureSetIndex =
        importObjectIndex;

    _stprintf_s(
        importProgressStep,
        _countof(importProgressStep),
        _T("Creating material for %s"),
        importProgressName);

    if (!UpdateMaxGLBProgress(
            importProgressPercent,
            importProgressStep))
    {
        triObject->DeleteThis();

        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Import cancelled by the user."));
        return FALSE;
    }

    if (!CreateStandardMaterialForPrimitive(
            primitive,
            sourceFilename,
            maxInterface,
            &importedMaterial,
            &hasBaseColorTexture,
            &hasNormalTexture,
            &hasOrmChannels,
            &hasEmissiveTexture,
            errorMessage,
            errorMessageCount))
    {
        triObject->DeleteThis();
        return FALSE;
    }

    INode* maxNode =
        maxInterface->CreateObjectNode(triObject);

    if (maxNode == NULL)
    {
        triObject->DeleteThis();

        if (importedMaterial != NULL)
        {
            importedMaterial->DeleteThis();
        }

        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("3ds Max could not create a scene node for the mesh."));
        return FALSE;
    }

    // The complete glTF world transform has already been baked into the
    // positions and normals. Keeping the Max node transform at identity
    // avoids an additional or incorrectly converted axis rotation.
    maxNode->SetNodeTM(
        maxInterface->GetTime(),
        Matrix3(1));

    TCHAR importedNodeName[256];

    BuildImportedNodeName(
        sourceNode,
        sourceMesh,
        primitiveIndex,
        importObjectIndex,
        importedNodeName,
        _countof(importedNodeName));

    maxNode->SetName(importedNodeName);

    if (importedMaterial != NULL)
    {
        AppendMaterialImportLog(_T("12: assigning material to node"));
        maxNode->SetMtl(importedMaterial);
        AppendMaterialImportLog(_T("13: material assigned to node"));
    }

    importStats->createdNodes.push_back(
        maxNode);

    ++importStats->objectCount;
    ++importStats->primitiveCount;

    importStats->totalVertexCount +=
        static_cast<int>(
            weldedPositions.size());

    importStats->totalFaceCount +=
        static_cast<int>(
            faceCount);

    if (hasUvChannel)
    {
        ++importStats->objectsWithUv;
    }

    if (hasNormals)
    {
        ++importStats->objectsWithNormals;
    }

    if (importedMaterial != NULL)
    {
        ++importStats->materials;
    }

    if (hasBaseColorTexture)
    {
        ++importStats->baseColorTextures;
    }

    if (hasNormalTexture)
    {
        ++importStats->normalTextures;
    }

    if (hasOrmChannels)
    {
        ++importStats->ormTextures;
    }

    if (hasEmissiveTexture)
    {
        ++importStats->emissiveTextures;
    }

    const int completedImportPercent =
        5 +
        (importStats->objectCount * 88) /
        (g_activeImportPrimitiveCount > 0
            ? g_activeImportPrimitiveCount
            : 1);

    if (!UpdateMaxGLBProgress(
            completedImportPercent,
            importProgressStep))
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Import cancelled by the user."));
        return FALSE;
    }

    return TRUE;
}



struct MaxGLBPrimitiveImportInfo
{
    cgltf_primitive* primitive;

    const cgltf_accessor* positions;
    const cgltf_accessor* texcoords;
    const cgltf_accessor* normals;

    cgltf_size faceCount;

    size_t uvBase;
    size_t normalBase;

    int materialId;

    BOOL hasUv;
    BOOL hasNormals;

    std::vector<int> sourceToWelded;

    MaxGLBPrimitiveImportInfo()
        : primitive(NULL)
        , positions(NULL)
        , texcoords(NULL)
        , normals(NULL)
        , faceCount(0)
        , uvBase(0)
        , normalBase(0)
        , materialId(0)
        , hasUv(FALSE)
        , hasNormals(FALSE)
    {
    }
};


static StdMat2* CreateDefaultImportedSubMaterial(
    int materialId)
{
    StdMat2* material =
        NewDefaultStdMat();

    if (material == NULL)
    {
        return NULL;
    }

    TCHAR materialName[128];

    _stprintf_s(
        materialName,
        _countof(materialName),
        _T("MaxGLB2016_Default_%d"),
        materialId + 1);

    material->SetName(materialName);

    material->SetDiffuse(
        Color(
            0.8f,
            0.8f,
            0.8f),
        0);

    return material;
}


static BOOL ImportMeshPrimitivesAsNode(
    cgltf_data* data,
    const TCHAR* sourceFilename,
    const cgltf_node* sourceNode,
    cgltf_mesh* sourceMesh,
    int importObjectIndex,
    Interface* maxInterface,
    MaxGLBImportStats* importStats,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (data == NULL ||
        sourceFilename == NULL ||
        sourceNode == NULL ||
        sourceMesh == NULL ||
        maxInterface == NULL ||
        importStats == NULL)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The importer received invalid multi-primitive mesh data."));
        return FALSE;
    }

    std::vector<MaxGLBPrimitiveImportInfo> primitiveInfos;

    primitiveInfos.reserve(
        static_cast<size_t>(
            sourceMesh->primitives_count));

    cgltf_size totalFaceCount = 0;
    size_t totalSourceVertexCount = 0;

    BOOL anyUv = FALSE;
    BOOL allHaveNormals = TRUE;

    for (cgltf_size sourcePrimitiveIndex = 0;
         sourcePrimitiveIndex <
            sourceMesh->primitives_count;
         ++sourcePrimitiveIndex)
    {
        cgltf_primitive* primitive =
            &sourceMesh->primitives[
                sourcePrimitiveIndex];

        if (primitive->type !=
            cgltf_primitive_type_triangles)
        {
            continue;
        }

        const cgltf_accessor* positions =
            cgltf_find_accessor(
                primitive,
                cgltf_attribute_type_position,
                0);

        if (positions == NULL ||
            positions->type !=
                cgltf_type_vec3 ||
            positions->count == 0)
        {
            continue;
        }

        if (positions->is_sparse)
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("Sparse POSITION accessors are not supported yet."));
            return FALSE;
        }

        MaxGLBPrimitiveImportInfo info;

        info.primitive =
            primitive;

        info.positions =
            positions;

        info.texcoords =
            cgltf_find_accessor(
                primitive,
                cgltf_attribute_type_texcoord,
                0);

        info.normals =
            cgltf_find_accessor(
                primitive,
                cgltf_attribute_type_normal,
                0);

        info.hasUv =
            info.texcoords != NULL;

        info.hasNormals =
            info.normals != NULL;

        if (info.hasUv)
        {
            if (info.texcoords->type !=
                    cgltf_type_vec2 ||
                info.texcoords->count !=
                    positions->count ||
                info.texcoords->is_sparse)
            {
                _tcscpy_s(
                    errorMessage,
                    errorMessageCount,
                    _T("A multi-primitive mesh contains incompatible TEXCOORD_0 data."));
                return FALSE;
            }

            anyUv =
                TRUE;
        }

        if (info.hasNormals)
        {
            if (info.normals->type !=
                    cgltf_type_vec3 ||
                info.normals->count !=
                    positions->count ||
                info.normals->is_sparse)
            {
                _tcscpy_s(
                    errorMessage,
                    errorMessageCount,
                    _T("A multi-primitive mesh contains incompatible NORMAL data."));
                return FALSE;
            }
        }
        else
        {
            allHaveNormals =
                FALSE;
        }

        const cgltf_size indexCount =
            primitive->indices != NULL
            ? primitive->indices->count
            : positions->count;

        if (indexCount < 3 ||
            (indexCount % 3) != 0)
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("A primitive index count is not a valid triangle list."));
            return FALSE;
        }

        if (primitive->indices != NULL &&
            primitive->indices->is_sparse)
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("Sparse index accessors are not supported yet."));
            return FALSE;
        }

        info.faceCount =
            indexCount / 3;

        if (positions->count >
                static_cast<cgltf_size>(
                    INT_MAX) ||
            info.faceCount >
                static_cast<cgltf_size>(
                    INT_MAX))
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("A primitive is too large for this 3ds Max importer."));
            return FALSE;
        }

        if (totalFaceCount >
                static_cast<cgltf_size>(
                    INT_MAX) -
                info.faceCount ||
            totalSourceVertexCount >
                static_cast<size_t>(
                    INT_MAX) -
                static_cast<size_t>(
                    positions->count))
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("The combined mesh is too large for this 3ds Max importer."));
            return FALSE;
        }

        info.materialId =
            static_cast<int>(
                primitiveInfos.size());

        info.uvBase =
            totalSourceVertexCount;

        info.normalBase =
            totalSourceVertexCount;

        totalFaceCount +=
            info.faceCount;

        totalSourceVertexCount +=
            static_cast<size_t>(
                positions->count);

        primitiveInfos.push_back(
            info);
    }

    if (primitiveInfos.empty())
    {
        return TRUE;
    }

    TCHAR importedNodeName[256];

    BuildImportedMeshNodeName(
        sourceNode,
        sourceMesh,
        importObjectIndex,
        importedNodeName,
        _countof(importedNodeName));

    TCHAR importProgressStep[384];

    _stprintf_s(
        importProgressStep,
        _countof(importProgressStep),
        _T("Importing %s (%d material primitives)"),
        importedNodeName,
        static_cast<int>(
            primitiveInfos.size()));

    const int startingPrimitiveCount =
        importStats->primitiveCount;

    const int importProgressPercent =
        5 +
        (startingPrimitiveCount * 88) /
        (g_activeImportPrimitiveCount > 0
            ? g_activeImportPrimitiveCount
            : 1);

    if (!UpdateMaxGLBProgress(
            importProgressPercent,
            importProgressStep))
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Import cancelled by the user."));
        return FALSE;
    }

    cgltf_float gltfWorld[16] =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    BuildGltfWorldMatrix(
        data,
        sourceNode,
        gltfWorld);

    std::vector<Point3> weldedPositions;
    std::map<MaxGLBPositionKey, int> weldedLookup;

    weldedPositions.reserve(
        totalSourceVertexCount);

    for (size_t primitiveInfoIndex = 0;
         primitiveInfoIndex <
            primitiveInfos.size();
         ++primitiveInfoIndex)
    {
        MaxGLBPrimitiveImportInfo& info =
            primitiveInfos[
                primitiveInfoIndex];

        info.sourceToWelded.resize(
            static_cast<size_t>(
                info.positions->count));

        for (cgltf_size sourceVertexIndex = 0;
             sourceVertexIndex <
                info.positions->count;
             ++sourceVertexIndex)
        {
            if ((sourceVertexIndex & 4095) == 0 &&
                !ContinueMaxGLBOperation(
                    errorMessage,
                    errorMessageCount))
            {
                return FALSE;
            }

            cgltf_float localPosition[3];

            if (!cgltf_accessor_read_float(
                    info.positions,
                    sourceVertexIndex,
                    localPosition,
                    3))
            {
                _tcscpy_s(
                    errorMessage,
                    errorMessageCount,
                    _T("A vertex position could not be read."));
                return FALSE;
            }

            cgltf_float worldPosition[3];

            TransformGltfPoint(
                gltfWorld,
                localPosition[0],
                localPosition[1],
                localPosition[2],
                worldPosition);

            const Point3 maxPosition =
                ConvertGltfLocalPositionToMax(
                    worldPosition);

            const MaxGLBPositionKey key(
                maxPosition);

            std::map<
                MaxGLBPositionKey,
                int>::iterator existing =
                    weldedLookup.find(key);

            int weldedIndex;

            if (existing !=
                weldedLookup.end())
            {
                weldedIndex =
                    existing->second;
            }
            else
            {
                if (weldedPositions.size() >=
                    static_cast<size_t>(
                        INT_MAX))
                {
                    _tcscpy_s(
                        errorMessage,
                        errorMessageCount,
                        _T("The welded mesh contains too many vertices."));
                    return FALSE;
                }

                weldedIndex =
                    static_cast<int>(
                        weldedPositions.size());

                weldedPositions.push_back(
                    maxPosition);

                weldedLookup.insert(
                    std::make_pair(
                        key,
                        weldedIndex));
            }

            info.sourceToWelded[
                static_cast<size_t>(
                    sourceVertexIndex)] =
                weldedIndex;
        }
    }

    TriObject* triObject =
        CreateNewTriObject();

    if (triObject == NULL)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("3ds Max could not create a combined TriObject."));
        return FALSE;
    }

    Mesh& maxMesh =
        triObject->GetMesh();

    maxMesh.setNumVerts(
        static_cast<int>(
            weldedPositions.size()));

    maxMesh.setNumFaces(
        static_cast<int>(
            totalFaceCount));

    for (size_t weldedIndex = 0;
         weldedIndex <
            weldedPositions.size();
         ++weldedIndex)
    {
        maxMesh.setVert(
            static_cast<int>(
                weldedIndex),
            weldedPositions[
                weldedIndex]);
    }

    if (anyUv)
    {
        maxMesh.setNumTVerts(
            static_cast<int>(
                totalSourceVertexCount));

        maxMesh.setNumTVFaces(
            static_cast<int>(
                totalFaceCount));
    }

    MeshNormalSpec* specifiedNormals =
        NULL;

    if (allHaveNormals)
    {
        maxMesh.SpecifyNormals();

        specifiedNormals =
            maxMesh.GetSpecifiedNormals();

        if (specifiedNormals == NULL)
        {
            triObject->DeleteThis();

            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("3ds Max could not allocate combined specified normals."));
            return FALSE;
        }

        specifiedNormals->SetParent(
            &maxMesh);

        if (!specifiedNormals->SetNumFaces(
                static_cast<int>(
                    totalFaceCount)) ||
            !specifiedNormals->SetNumNormals(
                static_cast<int>(
                    totalSourceVertexCount)))
        {
            triObject->DeleteThis();

            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("3ds Max could not allocate the combined normal data."));
            return FALSE;
        }
    }

    for (size_t primitiveInfoIndex = 0;
         primitiveInfoIndex <
            primitiveInfos.size();
         ++primitiveInfoIndex)
    {
        const MaxGLBPrimitiveImportInfo& info =
            primitiveInfos[
                primitiveInfoIndex];

        for (cgltf_size sourceVertexIndex = 0;
             sourceVertexIndex <
                info.positions->count;
             ++sourceVertexIndex)
        {
            const size_t combinedVertexIndex =
                info.uvBase +
                static_cast<size_t>(
                    sourceVertexIndex);

            if (anyUv)
            {
                UVVert importedUv(
                    0.0f,
                    0.0f,
                    0.0f);

                if (info.hasUv)
                {
                    cgltf_float texCoord[2];

                    if (!cgltf_accessor_read_float(
                            info.texcoords,
                            sourceVertexIndex,
                            texCoord,
                            2))
                    {
                        triObject->DeleteThis();

                        _tcscpy_s(
                            errorMessage,
                            errorMessageCount,
                            _T("A TEXCOORD_0 value could not be read."));
                        return FALSE;
                    }

                    importedUv =
                        UVVert(
                            static_cast<float>(
                                texCoord[0]),
                            static_cast<float>(
                                1.0f -
                                texCoord[1]),
                            0.0f);
                }

                maxMesh.tVerts[
                    static_cast<int>(
                        combinedVertexIndex)] =
                    importedUv;
            }

            if (allHaveNormals)
            {
                cgltf_float sourceNormal[3];

                if (!cgltf_accessor_read_float(
                        info.normals,
                        sourceVertexIndex,
                        sourceNormal,
                        3))
                {
                    triObject->DeleteThis();

                    _tcscpy_s(
                        errorMessage,
                        errorMessageCount,
                        _T("A NORMAL value could not be read."));
                    return FALSE;
                }

                specifiedNormals->Normal(
                    static_cast<int>(
                        info.normalBase +
                        static_cast<size_t>(
                            sourceVertexIndex))) =
                    TransformGltfNormalToMaxWorld(
                        gltfWorld,
                        sourceNormal);

                specifiedNormals->SetNormalExplicit(
                    static_cast<int>(
                        info.normalBase +
                        static_cast<size_t>(
                            sourceVertexIndex)),
                    true);
            }
        }
    }

    int destinationFaceIndex = 0;

    for (size_t primitiveInfoIndex = 0;
         primitiveInfoIndex <
            primitiveInfos.size();
         ++primitiveInfoIndex)
    {
        const MaxGLBPrimitiveImportInfo& info =
            primitiveInfos[
                primitiveInfoIndex];

        for (cgltf_size localFaceIndex = 0;
             localFaceIndex <
                info.faceCount;
             ++localFaceIndex)
        {
            if ((localFaceIndex & 4095) == 0 &&
                !ContinueMaxGLBOperation(
                    errorMessage,
                    errorMessageCount))
            {
                triObject->DeleteThis();
                return FALSE;
            }

            cgltf_size sourceIndices[3];

            if (info.primitive->indices != NULL)
            {
                sourceIndices[0] =
                    cgltf_accessor_read_index(
                        info.primitive->indices,
                        localFaceIndex * 3 + 0);

                sourceIndices[1] =
                    cgltf_accessor_read_index(
                        info.primitive->indices,
                        localFaceIndex * 3 + 1);

                sourceIndices[2] =
                    cgltf_accessor_read_index(
                        info.primitive->indices,
                        localFaceIndex * 3 + 2);
            }
            else
            {
                sourceIndices[0] =
                    localFaceIndex * 3 + 0;

                sourceIndices[1] =
                    localFaceIndex * 3 + 1;

                sourceIndices[2] =
                    localFaceIndex * 3 + 2;
            }

            if (sourceIndices[0] >=
                    info.positions->count ||
                sourceIndices[1] >=
                    info.positions->count ||
                sourceIndices[2] >=
                    info.positions->count)
            {
                triObject->DeleteThis();

                _tcscpy_s(
                    errorMessage,
                    errorMessageCount,
                    _T("A primitive contains an invalid vertex index."));
                return FALSE;
            }

            Face& face =
                maxMesh.faces[
                    destinationFaceIndex];

            face.setVerts(
                info.sourceToWelded[
                    static_cast<size_t>(
                        sourceIndices[0])],
                info.sourceToWelded[
                    static_cast<size_t>(
                        sourceIndices[1])],
                info.sourceToWelded[
                    static_cast<size_t>(
                        sourceIndices[2])]);

            face.setEdgeVisFlags(
                1,
                1,
                1);

            face.setSmGroup(1);

            face.setMatID(
                static_cast<MtlID>(
                    info.materialId));

            if (anyUv)
            {
                maxMesh.tvFace[
                    destinationFaceIndex]
                    .setTVerts(
                        static_cast<int>(
                            info.uvBase +
                            static_cast<size_t>(
                                sourceIndices[0])),
                        static_cast<int>(
                            info.uvBase +
                            static_cast<size_t>(
                                sourceIndices[1])),
                        static_cast<int>(
                            info.uvBase +
                            static_cast<size_t>(
                                sourceIndices[2])));
            }

            if (allHaveNormals)
            {
                MeshNormalFace& normalFace =
                    specifiedNormals->Face(
                        destinationFaceIndex);

                normalFace.SpecifyNormalID(
                    0,
                    static_cast<int>(
                        info.normalBase +
                        static_cast<size_t>(
                            sourceIndices[0])));

                normalFace.SpecifyNormalID(
                    1,
                    static_cast<int>(
                        info.normalBase +
                        static_cast<size_t>(
                            sourceIndices[1])));

                normalFace.SpecifyNormalID(
                    2,
                    static_cast<int>(
                        info.normalBase +
                        static_cast<size_t>(
                            sourceIndices[2])));
            }

            ++destinationFaceIndex;
        }
    }

    maxMesh.InvalidateGeomCache();
    maxMesh.InvalidateTopologyCache();

    if (allHaveNormals)
    {
        specifiedNormals->SetAllExplicit(
            true);

        specifiedNormals->CheckNormals();
    }
    else
    {
        maxMesh.buildNormals();
    }

    std::vector<Mtl*> importedSubMaterials;

    importedSubMaterials.resize(
        primitiveInfos.size(),
        NULL);

    int materialCount = 0;

    for (size_t primitiveInfoIndex = 0;
         primitiveInfoIndex <
            primitiveInfos.size();
         ++primitiveInfoIndex)
    {
        if (!ContinueMaxGLBOperation(
                errorMessage,
                errorMessageCount))
        {
            triObject->DeleteThis();
            return FALSE;
        }

        Mtl* importedMaterial =
            NULL;

        BOOL hasBaseColorTexture =
            FALSE;

        BOOL hasNormalTexture =
            FALSE;

        BOOL hasOrmChannels =
            FALSE;

        BOOL hasEmissiveTexture =
            FALSE;

        g_importTextureSetIndex =
            importObjectIndex;

        if (!CreateStandardMaterialForPrimitive(
                primitiveInfos[
                    primitiveInfoIndex]
                    .primitive,
                sourceFilename,
                maxInterface,
                &importedMaterial,
                &hasBaseColorTexture,
                &hasNormalTexture,
                &hasOrmChannels,
                &hasEmissiveTexture,
                errorMessage,
                errorMessageCount))
        {
            triObject->DeleteThis();

            for (size_t cleanupIndex = 0;
                 cleanupIndex <
                    importedSubMaterials.size();
                 ++cleanupIndex)
            {
                if (importedSubMaterials[
                        cleanupIndex] != NULL)
                {
                    importedSubMaterials[
                        cleanupIndex]
                        ->DeleteThis();
                }
            }

            return FALSE;
        }

        if (importedMaterial == NULL)
        {
            importedMaterial =
                CreateDefaultImportedSubMaterial(
                    static_cast<int>(
                        primitiveInfoIndex));

            if (importedMaterial == NULL)
            {
                triObject->DeleteThis();

                for (size_t cleanupIndex = 0;
                     cleanupIndex <
                        importedSubMaterials.size();
                     ++cleanupIndex)
                {
                    if (importedSubMaterials[
                            cleanupIndex] != NULL)
                    {
                        importedSubMaterials[
                            cleanupIndex]
                            ->DeleteThis();
                    }
                }

                _tcscpy_s(
                    errorMessage,
                    errorMessageCount,
                    _T("3ds Max could not create a default sub-material."));
                return FALSE;
            }
        }

        importedSubMaterials[
            primitiveInfoIndex] =
            importedMaterial;

        ++materialCount;

        if (hasBaseColorTexture)
        {
            ++importStats->baseColorTextures;
        }

        if (hasNormalTexture)
        {
            ++importStats->normalTextures;
        }

        if (hasOrmChannels)
        {
            ++importStats->ormTextures;
        }

        if (hasEmissiveTexture)
        {
            ++importStats->emissiveTextures;
        }
    }

    MultiMtl* multiMaterial =
        NewDefaultMultiMtl();

    if (multiMaterial == NULL)
    {
        triObject->DeleteThis();

        for (size_t cleanupIndex = 0;
             cleanupIndex <
                importedSubMaterials.size();
             ++cleanupIndex)
        {
            if (importedSubMaterials[
                    cleanupIndex] != NULL)
            {
                importedSubMaterials[
                    cleanupIndex]
                    ->DeleteThis();
            }
        }

        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("3ds Max could not create a Multi/Sub-Object material."));
        return FALSE;
    }

    TCHAR multiMaterialName[320];

    _stprintf_s(
        multiMaterialName,
        _countof(multiMaterialName),
        _T("%s_Multi"),
        importedNodeName);

    multiMaterial->SetName(
        multiMaterialName);

    multiMaterial->SetNumSubMtls(
        static_cast<int>(
            importedSubMaterials.size()));

    for (size_t materialIndex = 0;
         materialIndex <
            importedSubMaterials.size();
         ++materialIndex)
    {
        multiMaterial->SetSubMtl(
            static_cast<int>(
                materialIndex),
            importedSubMaterials[
                materialIndex]);
    }

    INode* maxNode =
        maxInterface->CreateObjectNode(
            triObject);

    if (maxNode == NULL)
    {
        triObject->DeleteThis();
        multiMaterial->DeleteThis();

        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("3ds Max could not create a scene node for the combined mesh."));
        return FALSE;
    }

    maxNode->SetNodeTM(
        maxInterface->GetTime(),
        Matrix3(1));

    maxNode->SetName(
        importedNodeName);

    maxNode->SetMtl(
        multiMaterial);

    importStats->createdNodes.push_back(
        maxNode);

    ++importStats->objectCount;

    importStats->primitiveCount +=
        static_cast<int>(
            primitiveInfos.size());

    importStats->totalVertexCount +=
        static_cast<int>(
            weldedPositions.size());

    importStats->totalFaceCount +=
        static_cast<int>(
            totalFaceCount);

    if (anyUv)
    {
        ++importStats->objectsWithUv;
    }

    if (allHaveNormals)
    {
        ++importStats->objectsWithNormals;
    }

    importStats->materials +=
        materialCount;

    const int completedImportPercent =
        5 +
        (importStats->primitiveCount * 88) /
        (g_activeImportPrimitiveCount > 0
            ? g_activeImportPrimitiveCount
            : 1);

    if (!UpdateMaxGLBProgress(
            completedImportPercent,
            importProgressStep))
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Import cancelled by the user."));
        return FALSE;
    }

    return TRUE;
}


static BOOL ImportNodeMeshesRecursive(
    cgltf_data* data,
    const TCHAR* sourceFilename,
    const cgltf_node* sourceNode,
    Interface* maxInterface,
    MaxGLBImportStats* importStats,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (sourceNode == NULL)
    {
        return TRUE;
    }

    if (sourceNode->mesh != NULL)
    {
        cgltf_mesh* sourceMesh =
            sourceNode->mesh;

        int validPrimitiveCount = 0;
        int singleValidPrimitiveIndex = -1;

        for (cgltf_size primitiveIndex = 0;
             primitiveIndex <
                sourceMesh->primitives_count;
             ++primitiveIndex)
        {
            cgltf_primitive* primitive =
                &sourceMesh->primitives[
                    primitiveIndex];

            const cgltf_accessor* positions =
                cgltf_find_accessor(
                    primitive,
                    cgltf_attribute_type_position,
                    0);

            if (primitive->type ==
                    cgltf_primitive_type_triangles &&
                positions != NULL &&
                positions->type ==
                    cgltf_type_vec3 &&
                positions->count > 0)
            {
                ++validPrimitiveCount;

                singleValidPrimitiveIndex =
                    static_cast<int>(
                        primitiveIndex);
            }
        }

        if (validPrimitiveCount > 1)
        {
            if (importStats->nextObjectIndex ==
                INT_MAX)
            {
                _tcscpy_s(
                    errorMessage,
                    errorMessageCount,
                    _T("The GLB contains too many mesh nodes."));
                return FALSE;
            }

            const int objectIndex =
                importStats->nextObjectIndex++;

            if (!ImportMeshPrimitivesAsNode(
                    data,
                    sourceFilename,
                    sourceNode,
                    sourceMesh,
                    objectIndex,
                    maxInterface,
                    importStats,
                    errorMessage,
                    errorMessageCount))
            {
                return FALSE;
            }
        }
        else if (validPrimitiveCount == 1)
        {
            if (singleValidPrimitiveIndex < 0 ||
                importStats->nextObjectIndex ==
                    INT_MAX)
            {
                _tcscpy_s(
                    errorMessage,
                    errorMessageCount,
                    _T("The GLB contains an invalid mesh primitive."));
                return FALSE;
            }

            const int objectIndex =
                importStats->nextObjectIndex++;

            if (!ImportPrimitiveAsNode(
                    data,
                    sourceFilename,
                    sourceNode,
                    sourceMesh,
                    &sourceMesh->primitives[
                        singleValidPrimitiveIndex],
                    singleValidPrimitiveIndex,
                    objectIndex,
                    maxInterface,
                    importStats,
                    errorMessage,
                    errorMessageCount))
            {
                return FALSE;
            }
        }
    }

    for (cgltf_size childIndex = 0;
         childIndex <
            sourceNode->children_count;
         ++childIndex)
    {
        if (!ImportNodeMeshesRecursive(
                data,
                sourceFilename,
                sourceNode->children[
                    childIndex],
                maxInterface,
                importStats,
                errorMessage,
                errorMessageCount))
        {
            return FALSE;
        }
    }

    return TRUE;
}


static void BuildImportedRootName(
    const cgltf_data* data,
    const TCHAR* sourceFilename,
    TCHAR* outputName,
    size_t outputNameCount)
{
    if (outputName == NULL ||
        outputNameCount == 0)
    {
        return;
    }

    TCHAR baseName[256];
    baseName[0] = _T('\0');

    GetFilenameOnly(
        sourceFilename,
        baseName,
        _countof(baseName));

    TCHAR* extension =
        _tcsrchr(
            baseName,
            _T('.'));

    if (extension != NULL)
    {
        *extension = _T('\0');
    }

    // The filename is more useful than a generic scene name such as
    // "Scene". Use the glTF scene name only when no filename is available.
    if (baseName[0] == _T('\0'))
    {
        const cgltf_scene* activeScene =
            data != NULL
            ? data->scene
            : NULL;

        if (activeScene == NULL &&
            data != NULL &&
            data->scenes_count > 0)
        {
            activeScene =
                &data->scenes[0];
        }

        if (activeScene != NULL &&
            activeScene->name != NULL &&
            activeScene->name[0] != '\0')
        {
            ConvertUtf8ToTchar(
                activeScene->name,
                baseName,
                _countof(baseName));
        }
    }

    if (baseName[0] == _T('\0'))
    {
        _tcscpy_s(
            baseName,
            _countof(baseName),
            _T("ImportedGLB"));
    }

    _stprintf_s(
        outputName,
        outputNameCount,
        _T("Root:%s"),
        baseName);
}


static BOOL CreateImportedParentNode(
    cgltf_data* data,
    const TCHAR* sourceFilename,
    Interface* maxInterface,
    MaxGLBImportStats* importStats,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (!g_activeImportSettings.addParentNode ||
        importStats == NULL ||
        importStats->createdNodes.size() <= 1)
    {
        return TRUE;
    }

    Object* dummyObject =
        static_cast<Object*>(
            CreateInstance(
                HELPER_CLASS_ID,
                Class_ID(DUMMY_CLASS_ID, 0)));

    if (dummyObject == NULL)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("3ds Max could not create the requested parent Dummy helper."));
        return FALSE;
    }

    INode* rootNode =
        maxInterface->CreateObjectNode(
            dummyObject);

    if (rootNode == NULL)
    {
        dummyObject->DeleteThis();

        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("3ds Max could not create the requested parent scene node."));
        return FALSE;
    }

    rootNode->SetNodeTM(
        maxInterface->GetTime(),
        Matrix3(1));

    TCHAR rootName[256];
    BuildImportedRootName(
        data,
        sourceFilename,
        rootName,
        _countof(rootName));

    rootNode->SetName(
        rootName);

    rootNode->SetWireColor(
        RGB(255, 204, 64));

    for (size_t nodeIndex = 0;
         nodeIndex < importStats->createdNodes.size();
         ++nodeIndex)
    {
        INode* childNode =
            importStats->createdNodes[nodeIndex];

        if (childNode != NULL &&
            childNode != rootNode)
        {
            rootNode->AttachChild(
                childNode,
                1);
        }
    }

    maxInterface->ClearNodeSelection(FALSE);
    maxInterface->SelectNode(rootNode, FALSE);

    importStats->createdParentNode =
        TRUE;

    return TRUE;
}


static BOOL ImportAllSceneMeshes(
    cgltf_data* data,
    const TCHAR* sourceFilename,
    Interface* maxInterface,
    MaxGLBImportStats* importStats,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (data == NULL ||
        sourceFilename == NULL ||
        maxInterface == NULL ||
        importStats == NULL)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The importer received invalid scene data."));
        return FALSE;
    }

    const cgltf_scene* activeScene =
        data->scene;

    if (activeScene == NULL &&
        data->scenes_count > 0)
    {
        activeScene =
            &data->scenes[0];
    }

    if (activeScene != NULL &&
        activeScene->nodes_count > 0)
    {
        for (cgltf_size rootIndex = 0;
             rootIndex < activeScene->nodes_count;
             ++rootIndex)
        {
            if (!ImportNodeMeshesRecursive(
                    data,
                    sourceFilename,
                    activeScene->nodes[rootIndex],
                    maxInterface,
                    importStats,
                    errorMessage,
                    errorMessageCount))
            {
                return FALSE;
            }
        }
    }
    else
    {
        // Files without a scene declaration are uncommon but legal enough
        // to salvage: import every root node and its descendants.
        for (cgltf_size nodeIndex = 0;
             nodeIndex < data->nodes_count;
             ++nodeIndex)
        {
            const cgltf_node* node =
                &data->nodes[nodeIndex];

            if (FindParentNode(data, node) != NULL)
            {
                continue;
            }

            if (!ImportNodeMeshesRecursive(
                    data,
                    sourceFilename,
                    node,
                    maxInterface,
                    importStats,
                    errorMessage,
                    errorMessageCount))
            {
                return FALSE;
            }
        }
    }

    if (importStats->objectCount == 0)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("No supported triangle mesh objects were found in the active GLB scene."));
        return FALSE;
    }

    if (!UpdateMaxGLBProgress(
            96,
            _T("Creating optional root node...")))
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Import cancelled by the user."));
        return FALSE;
    }

    if (!CreateImportedParentNode(
            data,
            sourceFilename,
            maxInterface,
            importStats,
            errorMessage,
            errorMessageCount))
    {
        return FALSE;
    }

    maxInterface->RedrawViews(
        maxInterface->GetTime());

    return TRUE;
}



class MaxGLBImporter : public SceneImport
{
public:
    int ExtCount()
    {
        return 1;
    }

    const TCHAR* Ext(int /*n*/)
    {
        return _T("GLB");
    }

    const TCHAR* LongDesc()
    {
        return _T("glTF Binary Scene");
    }

    const TCHAR* ShortDesc()
    {
        return _T("GLB Binary Scene");
    }

    const TCHAR* AuthorName()
    {
        return _T("Martin Hoeglund");
    }

    const TCHAR* CopyrightMessage()
    {
        return _T("Copyright (c) 2026 Martin Hoeglund");
    }

    const TCHAR* OtherMessage1()
    {
        return _T("");
    }

    const TCHAR* OtherMessage2()
    {
        return _T("");
    }

    unsigned int Version()
    {
        return 109;
    }

    void ShowAbout(HWND parentWindow)
    {
        MessageBox(
            parentWindow,
            _T("MaxGLB2016\nGeometry, UV and normal import milestone"),
            _T("About MaxGLB2016"),
            MB_OK | MB_ICONINFORMATION);
    }

    int DoImport(
        const TCHAR* filename,
        ImpInterface* /*importInterface*/,
        Interface* maxInterface,
        BOOL suppressPrompts = FALSE)
    {
        HWND parentWindow =
            maxInterface != NULL
            ? maxInterface->GetMAXHWnd()
            : NULL;

        FILE* file = NULL;

        errno_t openResult = _tfopen_s(
            &file,
            filename,
            _T("rb"));

        if (openResult != 0 || file == NULL)
        {
            if (!suppressPrompts)
            {
                ShowImportMessage(
                    parentWindow,
                    _T("The GLB file could not be opened."),
                    MB_ICONERROR);
            }

            return IMPEXP_FAIL;
        }

        if (_fseeki64(file, 0, SEEK_END) != 0)
        {
            fclose(file);

            if (!suppressPrompts)
            {
                ShowImportMessage(
                    parentWindow,
                    _T("The GLB file size could not be determined."),
                    MB_ICONERROR);
            }

            return IMPEXP_FAIL;
        }

        __int64 fileSize64 = _ftelli64(file);

        if (fileSize64 <= 0)
        {
            fclose(file);

            if (!suppressPrompts)
            {
                ShowImportMessage(
                    parentWindow,
                    _T("The selected GLB file is empty."),
                    MB_ICONERROR);
            }

            return IMPEXP_FAIL;
        }

        if (_fseeki64(file, 0, SEEK_SET) != 0)
        {
            fclose(file);
            return IMPEXP_FAIL;
        }

        size_t fileSize =
            static_cast<size_t>(fileSize64);

        void* fileBytes = malloc(fileSize);

        if (fileBytes == NULL)
        {
            fclose(file);

            if (!suppressPrompts)
            {
                ShowImportMessage(
                    parentWindow,
                    _T("Not enough memory to read the GLB file."),
                    MB_ICONERROR);
            }

            return IMPEXP_FAIL;
        }

        size_t bytesRead =
            fread(fileBytes, 1, fileSize, file);

        fclose(file);

        if (bytesRead != fileSize)
        {
            free(fileBytes);

            if (!suppressPrompts)
            {
                ShowImportMessage(
                    parentWindow,
                    _T("The GLB file could not be read completely."),
                    MB_ICONERROR);
            }

            return IMPEXP_FAIL;
        }

        cgltf_options options;
        ZeroMemory(&options, sizeof(options));

        cgltf_data* data = NULL;

        // JSON numbers always use a dot as decimal separator. 3ds Max can
        // run under a comma-decimal Windows locale, while cgltf 1.15 uses
        // the process C numeric locale during parsing. Preserve the user's
        // locale, force "C" only for cgltf_parse(), then restore it.
        char previousNumericLocale[128];
        previousNumericLocale[0] = '\0';

        const char* currentNumericLocale =
            setlocale(
                LC_NUMERIC,
                NULL);

        if (currentNumericLocale != NULL)
        {
            strncpy_s(
                previousNumericLocale,
                _countof(previousNumericLocale),
                currentNumericLocale,
                _TRUNCATE);
        }

        setlocale(
            LC_NUMERIC,
            "C");

        const cgltf_result parseResult =
            cgltf_parse(
                &options,
                fileBytes,
                static_cast<cgltf_size>(fileSize),
                &data);

        if (previousNumericLocale[0] != '\0')
        {
            setlocale(
                LC_NUMERIC,
                previousNumericLocale);
        }

        if (parseResult != cgltf_result_success ||
            data == NULL)
        {
            if (data != NULL)
            {
                cgltf_free(data);
            }

            free(fileBytes);

            if (!suppressPrompts)
            {
                TCHAR message[256];

                _stprintf_s(
                    message,
                    _countof(message),
                    _T("cgltf could not parse the selected GLB file.\n\n")
                    _T("Error code: %d"),
                    static_cast<int>(parseResult));

                ShowImportMessage(
                    parentWindow,
                    message,
                    MB_ICONERROR);
            }

            return IMPEXP_FAIL;
        }

        if (!AttachEmbeddedGlbBuffer(data))
        {
            cgltf_free(data);
            free(fileBytes);

            if (!suppressPrompts)
            {
                ShowImportMessage(
                    parentWindow,
                    _T("No supported embedded GLB geometry buffer was found."),
                    MB_ICONERROR);
            }

            return IMPEXP_FAIL;
        }

        cgltf_result validateResult =
            cgltf_validate(data);

        if (validateResult != cgltf_result_success)
        {
            cgltf_free(data);
            free(fileBytes);

            if (!suppressPrompts)
            {
                TCHAR message[256];

                _stprintf_s(
                    message,
                    _countof(message),
                    _T("The GLB structure did not pass cgltf validation.\n\n")
                    _T("Error code: %d"),
                    static_cast<int>(validateResult));

                ShowImportMessage(
                    parentWindow,
                    message,
                    MB_ICONERROR);
            }

            return IMPEXP_FAIL;
        }

        MaxGLBImportSettings importSettings =
            g_lastImportSettings;

        if (!suppressPrompts)
        {
            if (!ShowMaxGLBImportOptions(
                    parentWindow,
                    filename,
                    data,
                    &importSettings))
            {
                cgltf_free(data);
                free(fileBytes);
                return IMPEXP_CANCEL;
            }

            g_lastImportSettings =
                importSettings;
        }

        g_activeImportSettings =
            importSettings;

        g_activeImportSettings.appliedUniformScale =
            1.0f;

        g_activeImportData = data;
        g_splitOrmImagesThisImport.clear();
        g_unchangedImagesThisImport.clear();

        g_activeImportPrimitiveCount =
            CountImportableScenePrimitives(data);

        if (g_activeImportPrimitiveCount <= 0)
        {
            g_activeImportPrimitiveCount = 1;
        }

        BeginMaxGLBProgress(
            suppressPrompts
                ? NULL
                : maxInterface,
            _T("MaxGLB2016 Import"));

        UpdateMaxGLBProgress(
            1,
            _T("Preparing GLB scene..."));

        MaxGLBImportStats importStats;

        TCHAR importError[512];
        importError[0] = _T('\0');

        if (g_activeImportSettings.normalizeSize &&
            !ComputeNormalizedImportScale(
                data,
                g_activeImportSettings.rotationMatrix,
                g_activeImportSettings.normalizeTargetSize,
                &g_activeImportSettings.appliedUniformScale))
        {
            EndMaxGLBProgress();

            g_activeImportData = NULL;
            g_splitOrmImagesThisImport.clear();
            g_unchangedImagesThisImport.clear();

            cgltf_free(data);
            free(fileBytes);

            if (!suppressPrompts)
            {
                ShowImportMessage(
                    parentWindow,
                    _T("The model size could not be normalized because no valid geometry bounds were found."),
                    MB_ICONERROR);
            }

            return IMPEXP_FAIL;
        }

        UpdateMaxGLBProgress(
            3,
            _T("Calculating import transforms..."));

        // A single Hold transaction makes the complete import one Ctrl+Z
        // operation. Cancel restores the scene to its state before import.
        theHold.Begin();

        BOOL importSucceeded =
            ImportAllSceneMeshes(
                data,
                filename,
                maxInterface,
                &importStats,
                importError,
                _countof(importError));

        const BOOL importWasCancelled =
            MaxGLBOperationWasCancelled();

        if (importSucceeded &&
            !importWasCancelled)
        {
            UpdateMaxGLBProgress(
                100,
                _T("Import complete"));

            theHold.Accept(
                _T("Import GLB"));
        }
        else
        {
            theHold.Cancel();
        }

        EndMaxGLBProgress();

        g_activeImportData = NULL;
        g_splitOrmImagesThisImport.clear();
        g_unchangedImagesThisImport.clear();

        cgltf_free(data);
        free(fileBytes);

        if (!importSucceeded ||
            importWasCancelled)
        {
            if (maxInterface != NULL)
            {
                maxInterface->RedrawViews(
                    maxInterface->GetTime());
            }

            if (importWasCancelled)
            {
                return IMPEXP_CANCEL;
            }

            if (!suppressPrompts)
            {
                ShowImportMessage(
                    parentWindow,
                    importError,
                    MB_ICONERROR);
            }

            return IMPEXP_FAIL;
        }

        if (!suppressPrompts)
        {
            // The detailed success report is longer than 512 TCHARs.
            // VS2012's secure CRT terminates the process when _stprintf_s
            // receives a destination buffer that is too small.
            TCHAR message[2048];

            const int formatResult =
                _stprintf_s(
                message,
                _countof(message),
                _T("GLB scene imported successfully.\n\n")
                _T("Created Max objects: %d\n")
                _T("Triangle primitives: %d\n")
                _T("Welded vertices total: %d\n")
                _T("Triangles total: %d\n")
                _T("Objects with UV channel 1: %d\n")
                _T("Objects with explicit normals: %d\n")
                _T("Standard materials: %d\n")
                _T("Base-color textures: %d\n")
                _T("Normal textures: %d\n")
                _T("ORM texture sets: %d\n")
                _T("Emissive textures: %d\n\n")
                _T("Import behavior:\n")
                _T("- every mesh node in the active scene is imported\n")
                _T("- every glTF mesh node becomes one Max object\n")
                _T("- multiple primitives become Face Material IDs\n")
                _T("- Multi/Sub-Object materials are created automatically\n")
                _T("- original node names are preserved\n")
                _T("- full source hierarchy transforms are baked into geometry\n")
                _T("- standard glTF Y-up to Max Z-up conversion\n")
                _T("- import orientation: 3x3 rotation matrix\n")
                _T("- textures imported: %s\n")
                _T("- normalize size: %s\n")
                _T("- requested largest dimension: %.3f\n")
                _T("- applied uniform scale: %.6f\n")
                _T("- Root parent created: %s\n")
                _T("- TEXCOORD_0 imported to map channel 1\n")
                _T("- duplicate POSITION vertices welded safely\n")
                _T("- NORMAL imported as explicit Max normals"),
                importStats.objectCount,
                importStats.primitiveCount,
                importStats.totalVertexCount,
                importStats.totalFaceCount,
                importStats.objectsWithUv,
                importStats.objectsWithNormals,
                importStats.materials,
                importStats.baseColorTextures,
                importStats.normalTextures,
                importStats.ormTextures,
                importStats.emissiveTextures,
                g_activeImportSettings.importTextures
                    ? _T("yes")
                    : _T("no"),
                g_activeImportSettings.normalizeSize
                    ? _T("yes")
                    : _T("no"),
                g_activeImportSettings.normalizeTargetSize,
                g_activeImportSettings.appliedUniformScale,
                importStats.createdParentNode
                    ? _T("yes")
                    : _T("no"));

            if (formatResult < 0)
            {
                _tcscpy_s(
                    message,
                    _countof(message),
                    _T("The GLB scene and its materials were imported successfully."));
            }

            AppendMaterialImportLog(_T("15: success message formatted"));

            ShowImportMessage(
                parentWindow,
                message,
                MB_ICONINFORMATION);

            AppendMaterialImportLog(_T("16: success message closed"));
        }

        return IMPEXP_SUCCESS;
    }
};



enum MaxGLBExportTextureRole
{
    MAXGLB_TEXTURE_BASE_COLOR = 0,
    MAXGLB_TEXTURE_NORMAL = 1,
    MAXGLB_TEXTURE_ORM = 2,
    MAXGLB_TEXTURE_EMISSIVE = 3,
    MAXGLB_TEXTURE_COUNT = 4
};


struct MaxGLBExportImageData
{
    BOOL present;

    std::string name;
    std::string mimeType;

    std::vector<unsigned char> bytes;

    MaxGLBExportImageData()
        : present(FALSE)
    {
    }
};


struct MaxGLBExportMaterial
{
    BOOL present;
    BOOL doubleSided;
    int alphaMode;
    float alphaCutoff;
    DWORD alphaMetadataFlags;
    BOOL preservedOriginalOrm;

    float baseColor[4];

    std::string name;

    MaxGLBExportImageData textures[
        MAXGLB_TEXTURE_COUNT];

    MaxGLBExportMaterial()
        : present(FALSE)
        , doubleSided(FALSE)
        , alphaMode(MAXGLB_ALPHA_OPAQUE)
        , alphaCutoff(0.5f)
        , alphaMetadataFlags(0)
        , preservedOriginalOrm(FALSE)
    {
        baseColor[0] = 1.0f;
        baseColor[1] = 1.0f;
        baseColor[2] = 1.0f;
        baseColor[3] = 1.0f;
    }
};


struct MaxGLBExportMesh
{
    // One entry represents one glTF primitive.
    // Entries sharing nodeGroupIndex belong to the same Max object and are
    // written as multiple primitives of one glTF mesh.
    int nodeGroupIndex;
    int sourceMaterialId;

    std::string nodeName;
    std::string name;

    // glTF uses one shared index for POSITION, NORMAL and TEXCOORD_0.
    // We therefore export one vertex per Max face corner. This preserves
    // UV seams, hard edges and explicit normals without data loss.
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> texcoords;
    std::vector<unsigned int> indices;

    BOOL hasTexcoords;

    MaxGLBExportMaterial material;

    float minimum[3];
    float maximum[3];

    MaxGLBExportMesh()
        : nodeGroupIndex(-1)
        , sourceMaterialId(0)
        , hasTexcoords(FALSE)
    {
        minimum[0] = minimum[1] = minimum[2] = 0.0f;
        maximum[0] = maximum[1] = maximum[2] = 0.0f;
    }
};


struct MaxGLBExportVertexKey
{
    float positionX;
    float positionY;
    float positionZ;

    float normalX;
    float normalY;
    float normalZ;

    float uvX;
    float uvY;

    BOOL hasTexcoord;

    MaxGLBExportVertexKey(
        const Point3& position,
        const Point3& normal,
        BOOL hasUv,
        float u,
        float v)
        : positionX(position.x)
        , positionY(position.y)
        , positionZ(position.z)
        , normalX(normal.x)
        , normalY(normal.y)
        , normalZ(normal.z)
        , uvX(u)
        , uvY(v)
        , hasTexcoord(hasUv)
    {
    }

    bool operator<(
        const MaxGLBExportVertexKey& other) const
    {
#define MAXGLB_COMPARE_FIELD(fieldName) \
        if (fieldName < other.fieldName) return true; \
        if (fieldName > other.fieldName) return false

        MAXGLB_COMPARE_FIELD(positionX);
        MAXGLB_COMPARE_FIELD(positionY);
        MAXGLB_COMPARE_FIELD(positionZ);

        MAXGLB_COMPARE_FIELD(normalX);
        MAXGLB_COMPARE_FIELD(normalY);
        MAXGLB_COMPARE_FIELD(normalZ);

        if (hasTexcoord < other.hasTexcoord)
        {
            return true;
        }

        if (hasTexcoord > other.hasTexcoord)
        {
            return false;
        }

        if (hasTexcoord)
        {
            MAXGLB_COMPARE_FIELD(uvX);
            MAXGLB_COMPARE_FIELD(uvY);
        }

#undef MAXGLB_COMPARE_FIELD

        return false;
    }
};


struct MaxGLBExportLayout
{
    size_t positionOffset;
    size_t positionLength;
    size_t normalOffset;
    size_t normalLength;
    size_t texcoordOffset;
    size_t texcoordLength;
    size_t indexOffset;
    size_t indexLength;

    size_t positionBufferView;
    size_t normalBufferView;
    size_t texcoordBufferView;
    size_t indexBufferView;

    size_t positionAccessor;
    size_t normalAccessor;
    size_t texcoordAccessor;
    size_t indexAccessor;

    BOOL hasMaterial;
    size_t materialIndex;

    BOOL hasTexture[MAXGLB_TEXTURE_COUNT];
    size_t imageOffset[MAXGLB_TEXTURE_COUNT];
    size_t imageLength[MAXGLB_TEXTURE_COUNT];
    size_t imageBufferView[MAXGLB_TEXTURE_COUNT];
    size_t textureIndex[MAXGLB_TEXTURE_COUNT];
    size_t imageIndex[MAXGLB_TEXTURE_COUNT];

    MaxGLBExportLayout()
        : positionOffset(0)
        , positionLength(0)
        , normalOffset(0)
        , normalLength(0)
        , texcoordOffset(0)
        , texcoordLength(0)
        , indexOffset(0)
        , indexLength(0)
        , positionBufferView(0)
        , normalBufferView(0)
        , texcoordBufferView(0)
        , indexBufferView(0)
        , positionAccessor(0)
        , normalAccessor(0)
        , texcoordAccessor(0)
        , indexAccessor(0)
        , hasMaterial(FALSE)
        , materialIndex(0)
    {
        for (int roleIndex = 0;
             roleIndex < MAXGLB_TEXTURE_COUNT;
             ++roleIndex)
        {
            hasTexture[roleIndex] = FALSE;
            imageOffset[roleIndex] = 0;
            imageLength[roleIndex] = 0;
            imageBufferView[roleIndex] = 0;
            textureIndex[roleIndex] = 0;
            imageIndex[roleIndex] = 0;
        }
    }
};




static std::string ConvertTcharToUtf8(
    const TCHAR* textValue)
{
    if (textValue == NULL)
    {
        return std::string();
    }

#if defined(UNICODE) || defined(_UNICODE)
    const int requiredBytes =
        WideCharToMultiByte(
            CP_UTF8,
            0,
            textValue,
            -1,
            NULL,
            0,
            NULL,
            NULL);

    if (requiredBytes <= 1)
    {
        return std::string();
    }

    std::vector<char> utf8Bytes(
        static_cast<size_t>(requiredBytes));

    if (WideCharToMultiByte(
            CP_UTF8,
            0,
            textValue,
            -1,
            &utf8Bytes[0],
            requiredBytes,
            NULL,
            NULL) == 0)
    {
        return std::string();
    }

    return std::string(&utf8Bytes[0]);
#else
    return std::string(textValue);
#endif
}


static std::string EscapeJsonString(
    const std::string& sourceText)
{
    std::ostringstream output;

    for (size_t characterIndex = 0;
         characterIndex < sourceText.size();
         ++characterIndex)
    {
        const unsigned char character =
            static_cast<unsigned char>(
                sourceText[characterIndex]);

        switch (character)
        {
        case '\"':
            output << "\\\"";
            break;

        case '\\':
            output << "\\\\";
            break;

        case '\b':
            output << "\\b";
            break;

        case '\f':
            output << "\\f";
            break;

        case '\n':
            output << "\\n";
            break;

        case '\r':
            output << "\\r";
            break;

        case '\t':
            output << "\\t";
            break;

        default:
            if (character < 0x20)
            {
                output
                    << "\\u"
                    << std::hex
                    << std::setw(4)
                    << std::setfill('0')
                    << static_cast<unsigned int>(character)
                    << std::dec
                    << std::setfill(' ');
            }
            else
            {
                output << static_cast<char>(character);
            }
            break;
        }
    }

    return output.str();
}


static Point3 ConvertMaxWorldPositionToGltf(
    const Point3& maxPosition)
{
    // 3ds Max: right-handed, Z-up.
    // glTF:    right-handed, Y-up.
    return Point3(
        maxPosition.x,
        maxPosition.z,
        -maxPosition.y);
}


static Point3 ConvertMaxWorldNormalToGltf(
    const Point3& maxNormal)
{
    Point3 gltfNormal(
        maxNormal.x,
        maxNormal.z,
        -maxNormal.y);

    const float lengthSquared =
        DotProd(gltfNormal, gltfNormal);

    if (lengthSquared > 1.0e-20f)
    {
        gltfNormal /=
            sqrtf(lengthSquared);
    }
    else
    {
        gltfNormal =
            Point3(0.0f, 1.0f, 0.0f);
    }

    return gltfNormal;
}


static float GetMatrixDeterminant3x3(
    const Matrix3& matrix)
{
    const Point3 row0 = matrix.GetRow(0);
    const Point3 row1 = matrix.GetRow(1);
    const Point3 row2 = matrix.GetRow(2);

    return DotProd(
        row0,
        CrossProd(row1, row2));
}


static Point3 TransformNormalToWorld(
    const Point3& localNormal,
    const Matrix3& objectTransform)
{
    // Max uses row-vector transforms. A normal is transformed by the
    // inverse transpose of the linear 3x3 part. The cofactor rows below
    // form that matrix directly.
    const Point3 row0 =
        objectTransform.GetRow(0);

    const Point3 row1 =
        objectTransform.GetRow(1);

    const Point3 row2 =
        objectTransform.GetRow(2);

    const float determinant =
        DotProd(
            row0,
            CrossProd(row1, row2));

    Point3 worldNormal;

    if (fabsf(determinant) > 1.0e-20f)
    {
        worldNormal =
            (
                localNormal.x * CrossProd(row1, row2) +
                localNormal.y * CrossProd(row2, row0) +
                localNormal.z * CrossProd(row0, row1)
            ) / determinant;
    }
    else
    {
        // Degenerate transforms do not have an inverse. Use the linear
        // transform as a safe fallback and normalize afterwards.
        worldNormal =
            localNormal.x * row0 +
            localNormal.y * row1 +
            localNormal.z * row2;
    }

    const float lengthSquared =
        DotProd(worldNormal, worldNormal);

    if (lengthSquared > 1.0e-20f)
    {
        worldNormal /=
            sqrtf(lengthSquared);
    }
    else
    {
        worldNormal =
            Point3(0.0f, 0.0f, 1.0f);
    }

    return worldNormal;
}


static void AppendBinaryBytes(
    std::vector<unsigned char>& output,
    const void* sourceData,
    size_t byteCount)
{
    if (sourceData == NULL || byteCount == 0)
    {
        return;
    }

    const unsigned char* sourceBytes =
        static_cast<const unsigned char*>(
            sourceData);

    output.insert(
        output.end(),
        sourceBytes,
        sourceBytes + byteCount);
}


static void PadBinaryToFourBytes(
    std::vector<unsigned char>& output)
{
    while ((output.size() % 4) != 0)
    {
        output.push_back(0);
    }
}


static float ClampUnitFloat(
    float value)
{
    if (value < 0.0f)
    {
        return 0.0f;
    }

    if (value > 1.0f)
    {
        return 1.0f;
    }

    return value;
}


struct MaxGLBDecodedImage
{
    UINT width;
    UINT height;

    std::vector<unsigned char> rgba;

    MaxGLBDecodedImage()
        : width(0)
        , height(0)
    {
    }
};


static BOOL ReadWholeFile(
    const TCHAR* filePath,
    std::vector<unsigned char>& outputBytes,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    outputBytes.clear();

    if (filePath == NULL ||
        filePath[0] == _T('\0'))
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("A bitmap has no file path."));
        return FALSE;
    }

    FILE* inputFile = NULL;

    if (_tfopen_s(
            &inputFile,
            filePath,
            _T("rb")) != 0 ||
        inputFile == NULL)
    {
        _stprintf_s(
            errorMessage,
            errorMessageCount,
            _T("A bitmap could not be opened:\n%s"),
            filePath);
        return FALSE;
    }

    if (_fseeki64(
            inputFile,
            0,
            SEEK_END) != 0)
    {
        fclose(inputFile);

        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("A bitmap size could not be determined."));
        return FALSE;
    }

    const __int64 fileSize =
        _ftelli64(inputFile);

    if (fileSize <= 0 ||
        static_cast<unsigned __int64>(fileSize) >
            static_cast<unsigned __int64>(SIZE_MAX))
    {
        fclose(inputFile);

        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("A bitmap is empty or too large."));
        return FALSE;
    }

    if (_fseeki64(
            inputFile,
            0,
            SEEK_SET) != 0)
    {
        fclose(inputFile);

        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("A bitmap could not be rewound."));
        return FALSE;
    }

    outputBytes.resize(
        static_cast<size_t>(fileSize));

    const size_t bytesRead =
        fread(
            &outputBytes[0],
            1,
            outputBytes.size(),
            inputFile);

    fclose(inputFile);

    if (bytesRead != outputBytes.size())
    {
        outputBytes.clear();

        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("A bitmap could not be read completely."));
        return FALSE;
    }

    return TRUE;
}


static BOOL DetectGltfImageMimeType(
    const std::vector<unsigned char>& imageBytes,
    std::string& outputMimeType)
{
    outputMimeType.clear();

    static const unsigned char pngSignature[8] =
    {
        0x89,
        0x50,
        0x4E,
        0x47,
        0x0D,
        0x0A,
        0x1A,
        0x0A
    };

    if (imageBytes.size() >= 8 &&
        memcmp(
            &imageBytes[0],
            pngSignature,
            8) == 0)
    {
        outputMimeType =
            "image/png";
        return TRUE;
    }

    if (imageBytes.size() >= 3 &&
        imageBytes[0] == 0xFF &&
        imageBytes[1] == 0xD8 &&
        imageBytes[2] == 0xFF)
    {
        outputMimeType =
            "image/jpeg";
        return TRUE;
    }

    return FALSE;
}


static BOOL DecodeImageWithWic(
    const std::vector<unsigned char>& encodedBytes,
    MaxGLBDecodedImage* outputImage,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (outputImage == NULL ||
        encodedBytes.empty() ||
        encodedBytes.size() >
            static_cast<size_t>(UINT_MAX))
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Invalid image data was supplied to Windows Imaging Component."));
        return FALSE;
    }

    HRESULT initializeResult =
        CoInitializeEx(
            NULL,
            COINIT_APARTMENTTHREADED);

    const BOOL shouldUninitialize =
        SUCCEEDED(initializeResult);

    if (FAILED(initializeResult) &&
        initializeResult != RPC_E_CHANGED_MODE)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("COM could not be initialized for texture conversion."));
        return FALSE;
    }

    IWICImagingFactory* factory = NULL;
    IWICStream* stream = NULL;
    IWICBitmapDecoder* decoder = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* converter = NULL;

    HRESULT result =
        CoCreateInstance(
            CLSID_WICImagingFactory,
            NULL,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));

    if (SUCCEEDED(result))
    {
        result =
            factory->CreateStream(
                &stream);
    }

    if (SUCCEEDED(result))
    {
        result =
            stream->InitializeFromMemory(
                const_cast<BYTE*>(
                    reinterpret_cast<const BYTE*>(
                        &encodedBytes[0])),
                static_cast<DWORD>(
                    encodedBytes.size()));
    }

    if (SUCCEEDED(result))
    {
        result =
            factory->CreateDecoderFromStream(
                stream,
                NULL,
                WICDecodeMetadataCacheOnLoad,
                &decoder);
    }

    if (SUCCEEDED(result))
    {
        result =
            decoder->GetFrame(
                0,
                &frame);
    }

    if (SUCCEEDED(result))
    {
        result =
            factory->CreateFormatConverter(
                &converter);
    }

    if (SUCCEEDED(result))
    {
        result =
            converter->Initialize(
                frame,
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                NULL,
                0.0,
                WICBitmapPaletteTypeCustom);
    }

    UINT width = 0;
    UINT height = 0;

    if (SUCCEEDED(result))
    {
        result =
            converter->GetSize(
                &width,
                &height);
    }

    if (SUCCEEDED(result) &&
        (width == 0 ||
         height == 0 ||
         width > UINT_MAX / 4 ||
         height > UINT_MAX / (width * 4)))
    {
        result = E_FAIL;
    }

    if (SUCCEEDED(result))
    {
        outputImage->width =
            width;

        outputImage->height =
            height;

        outputImage->rgba.resize(
            static_cast<size_t>(width) *
            static_cast<size_t>(height) *
            4);

        result =
            converter->CopyPixels(
                NULL,
                width * 4,
                static_cast<UINT>(
                    outputImage->rgba.size()),
                reinterpret_cast<BYTE*>(
                    &outputImage->rgba[0]));
    }

    if (converter != NULL)
    {
        converter->Release();
    }

    if (frame != NULL)
    {
        frame->Release();
    }

    if (decoder != NULL)
    {
        decoder->Release();
    }

    if (stream != NULL)
    {
        stream->Release();
    }

    if (factory != NULL)
    {
        factory->Release();
    }

    if (shouldUninitialize)
    {
        CoUninitialize();
    }

    if (FAILED(result))
    {
        outputImage->width = 0;
        outputImage->height = 0;
        outputImage->rgba.clear();

        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("A material bitmap could not be decoded."));
        return FALSE;
    }

    return TRUE;
}


static BOOL EncodeRgbaPngWithWic(
    const MaxGLBDecodedImage& image,
    std::vector<unsigned char>& outputBytes,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    outputBytes.clear();

    if (image.width == 0 ||
        image.height == 0 ||
        image.rgba.size() !=
            static_cast<size_t>(image.width) *
            static_cast<size_t>(image.height) *
            4)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Invalid RGBA image data was supplied for PNG encoding."));
        return FALSE;
    }

    // The Windows 7 / VS2012 WIC PNG encoder used by 3ds Max 2016
    // commonly negotiates 32bppBGRA rather than 32bppRGBA.
    // Convert explicitly instead of rejecting the negotiated format.
    std::vector<unsigned char> bgraPixels;
    bgraPixels.resize(image.rgba.size());

    const size_t pixelCount =
        static_cast<size_t>(image.width) *
        static_cast<size_t>(image.height);

    for (size_t pixelIndex = 0;
         pixelIndex < pixelCount;
         ++pixelIndex)
    {
        const size_t sourceIndex =
            pixelIndex * 4;

        bgraPixels[sourceIndex + 0] =
            image.rgba[sourceIndex + 2];

        bgraPixels[sourceIndex + 1] =
            image.rgba[sourceIndex + 1];

        bgraPixels[sourceIndex + 2] =
            image.rgba[sourceIndex + 0];

        bgraPixels[sourceIndex + 3] =
            image.rgba[sourceIndex + 3];
    }

    HRESULT initializeResult =
        CoInitializeEx(
            NULL,
            COINIT_APARTMENTTHREADED);

    const BOOL shouldUninitialize =
        SUCCEEDED(initializeResult);

    if (FAILED(initializeResult) &&
        initializeResult != RPC_E_CHANGED_MODE)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("COM could not be initialized for PNG encoding."));
        return FALSE;
    }

    IStream* memoryStream = NULL;
    IWICImagingFactory* factory = NULL;
    IWICStream* wicStream = NULL;
    IWICBitmapEncoder* encoder = NULL;
    IWICBitmapFrameEncode* frame = NULL;
    IPropertyBag2* propertyBag = NULL;

    const TCHAR* failedStage =
        _T("creating the memory stream");

    HRESULT result =
        CreateStreamOnHGlobal(
            NULL,
            TRUE,
            &memoryStream);

    if (SUCCEEDED(result))
    {
        failedStage =
            _T("creating the WIC imaging factory");

        result =
            CoCreateInstance(
                CLSID_WICImagingFactory,
                NULL,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory));
    }

    if (SUCCEEDED(result))
    {
        failedStage =
            _T("creating the WIC output stream");

        result =
            factory->CreateStream(
                &wicStream);
    }

    if (SUCCEEDED(result))
    {
        failedStage =
            _T("initializing the WIC output stream");

        result =
            wicStream->InitializeFromIStream(
                memoryStream);
    }

    if (SUCCEEDED(result))
    {
        failedStage =
            _T("creating the PNG encoder");

        result =
            factory->CreateEncoder(
                GUID_ContainerFormatPng,
                NULL,
                &encoder);
    }

    if (SUCCEEDED(result))
    {
        failedStage =
            _T("initializing the PNG encoder");

        result =
            encoder->Initialize(
                wicStream,
                WICBitmapEncoderNoCache);
    }

    if (SUCCEEDED(result))
    {
        failedStage =
            _T("creating the PNG frame");

        result =
            encoder->CreateNewFrame(
                &frame,
                &propertyBag);
    }

    if (SUCCEEDED(result))
    {
        failedStage =
            _T("initializing the PNG frame");

        result =
            frame->Initialize(
                propertyBag);
    }

    if (SUCCEEDED(result))
    {
        failedStage =
            _T("setting the PNG dimensions");

        result =
            frame->SetSize(
                image.width,
                image.height);
    }

    WICPixelFormatGUID pixelFormat =
        GUID_WICPixelFormat32bppBGRA;

    if (SUCCEEDED(result))
    {
        failedStage =
            _T("setting the PNG pixel format");

        result =
            frame->SetPixelFormat(
                &pixelFormat);
    }

    if (SUCCEEDED(result) &&
        !IsEqualGUID(
            pixelFormat,
            GUID_WICPixelFormat32bppBGRA))
    {
        failedStage =
            _T("negotiating a supported 32-bit PNG pixel format");

        result = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    }

    if (SUCCEEDED(result))
    {
        failedStage =
            _T("writing PNG pixels");

        result =
            frame->WritePixels(
                image.height,
                image.width * 4,
                static_cast<UINT>(
                    bgraPixels.size()),
                reinterpret_cast<BYTE*>(
                    &bgraPixels[0]));
    }

    if (SUCCEEDED(result))
    {
        failedStage =
            _T("committing the PNG frame");

        result =
            frame->Commit();
    }

    if (SUCCEEDED(result))
    {
        failedStage =
            _T("committing the PNG encoder");

        result =
            encoder->Commit();
    }

    if (SUCCEEDED(result))
    {
        failedStage =
            _T("reading the encoded PNG stream");

        STATSTG streamStats;

        result =
            memoryStream->Stat(
                &streamStats,
                STATFLAG_NONAME);

        if (SUCCEEDED(result))
        {
            const ULONGLONG streamSize =
                streamStats.cbSize.QuadPart;

            if (streamSize == 0 ||
                streamSize >
                    static_cast<ULONGLONG>(SIZE_MAX) ||
                streamSize >
                    static_cast<ULONGLONG>(ULONG_MAX))
            {
                result = E_FAIL;
            }
            else
            {
                LARGE_INTEGER streamStart;
                streamStart.QuadPart = 0;

                result =
                    memoryStream->Seek(
                        streamStart,
                        STREAM_SEEK_SET,
                        NULL);

                if (SUCCEEDED(result))
                {
                    outputBytes.resize(
                        static_cast<size_t>(
                            streamSize));

                    ULONG bytesRead = 0;

                    result =
                        memoryStream->Read(
                            &outputBytes[0],
                            static_cast<ULONG>(
                                outputBytes.size()),
                            &bytesRead);

                    if (SUCCEEDED(result) &&
                        bytesRead !=
                            outputBytes.size())
                    {
                        result = E_FAIL;
                    }
                }
            }
        }
    }

    if (propertyBag != NULL)
    {
        propertyBag->Release();
    }

    if (frame != NULL)
    {
        frame->Release();
    }

    if (encoder != NULL)
    {
        encoder->Release();
    }

    if (wicStream != NULL)
    {
        wicStream->Release();
    }

    if (factory != NULL)
    {
        factory->Release();
    }

    if (memoryStream != NULL)
    {
        memoryStream->Release();
    }

    if (shouldUninitialize)
    {
        CoUninitialize();
    }

    if (FAILED(result))
    {
        outputBytes.clear();

        _stprintf_s(
            errorMessage,
            errorMessageCount,
            _T("An RGBA texture could not be encoded as PNG.\n\n")
            _T("WIC stage: %s\n")
            _T("HRESULT: 0x%08X"),
            failedStage,
            static_cast<unsigned int>(result));

        return FALSE;
    }

    return TRUE;
}


static BitmapTex* GetDirectBitmapFromStandardSlot(
    Mtl* material,
    StdMat* standardMaterial,
    int standardMapId)
{
    if (material == NULL ||
        standardMaterial == NULL ||
        !standardMaterial->MapEnabled(
            standardMapId))
    {
        return NULL;
    }

    Texmap* map =
        material->GetSubTexmap(
            standardMapId);

    if (map == NULL ||
        map->ClassID() !=
            Class_ID(BMTEX_CLASS_ID, 0))
    {
        return NULL;
    }

    return static_cast<BitmapTex*>(map);
}


static BOOL LoadBitmapImage(
    BitmapTex* bitmapTexture,
    MaxGLBExportImageData* outputImage,
    const std::string& imageName,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (outputImage == NULL)
    {
        return TRUE;
    }

    outputImage->present = FALSE;
    outputImage->bytes.clear();
    outputImage->mimeType.clear();
    outputImage->name.clear();

    if (bitmapTexture == NULL)
    {
        return TRUE;
    }

    if (!ReadWholeFile(
            bitmapTexture->GetMapName(),
            outputImage->bytes,
            errorMessage,
            errorMessageCount))
    {
        return FALSE;
    }

    if (!DetectGltfImageMimeType(
            outputImage->bytes,
            outputImage->mimeType))
    {
        outputImage->bytes.clear();

        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("A material bitmap is not PNG or JPEG."));
        return FALSE;
    }

    outputImage->present =
        TRUE;

    outputImage->name =
        imageName;

    return TRUE;
}


static BOOL FileExistsForExport(
    const TCHAR* filePath)
{
    if (filePath == NULL ||
        filePath[0] == _T('\0'))
    {
        return FALSE;
    }

    const DWORD attributes =
        GetFileAttributes(filePath);

    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}


static BOOL BuildSiblingImportedOrmPath(
    BitmapTex* sourceBitmap,
    TCHAR* outputPath,
    size_t outputPathCount)
{
    if (sourceBitmap == NULL ||
        outputPath == NULL ||
        outputPathCount == 0)
    {
        return FALSE;
    }

    const TCHAR* sourcePath =
        sourceBitmap->GetMapName();

    if (sourcePath == NULL ||
        sourcePath[0] == _T('\0'))
    {
        return FALSE;
    }

    TCHAR directory[MAX_PATH];
    _tcscpy_s(
        directory,
        _countof(directory),
        sourcePath);

    TCHAR* lastBackslash =
        _tcsrchr(directory, _T('\\'));

    TCHAR* lastForwardSlash =
        _tcsrchr(directory, _T('/'));

    TCHAR* lastSlash =
        lastBackslash;

    if (lastForwardSlash != NULL &&
        (lastSlash == NULL ||
         lastForwardSlash > lastSlash))
    {
        lastSlash =
            lastForwardSlash;
    }

    if (lastSlash == NULL)
    {
        return FALSE;
    }

    const TCHAR* sourceFileName =
        lastSlash + 1;

    const TCHAR* lastUnderscore =
        _tcsrchr(sourceFileName, _T('_'));

    const TCHAR* lastDot =
        _tcsrchr(sourceFileName, _T('.'));

    TCHAR indexSuffix[32];
    _tcscpy_s(
        indexSuffix,
        _countof(indexSuffix),
        _T("_0"));

    if (lastUnderscore != NULL &&
        lastDot != NULL &&
        lastUnderscore < lastDot)
    {
        const size_t suffixLength =
            static_cast<size_t>(
                lastDot - lastUnderscore);

        if (suffixLength > 1 &&
            suffixLength < _countof(indexSuffix))
        {
            _tcsncpy_s(
                indexSuffix,
                _countof(indexSuffix),
                lastUnderscore,
                suffixLength);

            indexSuffix[suffixLength] =
                _T('\0');
        }
    }

    *(lastSlash + 1) =
        _T('\0');

    if (_stprintf_s(
            outputPath,
            outputPathCount,
            _T("%sMaxGLB_OcclusionRoughnessMetallic%s.png"),
            directory,
            indexSuffix) < 0)
    {
        return FALSE;
    }

    return FileExistsForExport(outputPath);
}


static BOOL TryReuseOriginalImportedOrm(
    BitmapTex* occlusionBitmap,
    BitmapTex* glossinessBitmap,
    BitmapTex* metallicBitmap,
    const std::string& imageName,
    MaxGLBExportImageData* outputImage,
    BOOL* outReusedOriginal,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (outReusedOriginal != NULL)
    {
        *outReusedOriginal = FALSE;
    }

    if (outputImage == NULL)
    {
        return TRUE;
    }

    BitmapTex* candidateBitmaps[3] =
    {
        occlusionBitmap,
        glossinessBitmap,
        metallicBitmap
    };

    TCHAR packedOrmPath[MAX_PATH];
    packedOrmPath[0] = _T('\0');

    BOOL foundPackedOrm = FALSE;

    for (int candidateIndex = 0;
         candidateIndex < 3;
         ++candidateIndex)
    {
        if (BuildSiblingImportedOrmPath(
                candidateBitmaps[candidateIndex],
                packedOrmPath,
                _countof(packedOrmPath)))
        {
            foundPackedOrm = TRUE;
            break;
        }
    }

    if (!foundPackedOrm)
    {
        return TRUE;
    }

    outputImage->present = FALSE;
    outputImage->bytes.clear();
    outputImage->mimeType.clear();
    outputImage->name.clear();

    if (!ReadWholeFile(
            packedOrmPath,
            outputImage->bytes,
            errorMessage,
            errorMessageCount))
    {
        return FALSE;
    }

    if (!DetectGltfImageMimeType(
            outputImage->bytes,
            outputImage->mimeType))
    {
        outputImage->bytes.clear();

        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The preserved packed ORM image is not PNG or JPEG."));
        return FALSE;
    }

    outputImage->present =
        TRUE;

    outputImage->name =
        imageName;

    if (outReusedOriginal != NULL)
    {
        *outReusedOriginal =
            TRUE;
    }

    return TRUE;
}


static unsigned char GetGrayValue(
    const MaxGLBDecodedImage& image,
    size_t pixelIndex)
{
    const size_t rgbaIndex =
        pixelIndex * 4;

    const unsigned int red =
        image.rgba[rgbaIndex + 0];

    const unsigned int green =
        image.rgba[rgbaIndex + 1];

    const unsigned int blue =
        image.rgba[rgbaIndex + 2];

    return static_cast<unsigned char>(
        (red + green + blue) / 3);
}


static BOOL CaptureMaterialMaps(
    Mtl* nodeMaterial,
    const TCHAR* fallbackMaterialName,
    TimeValue timeValue,
    const MaxGLBExportSettings& exportSettings,
    MaxGLBExportMaterial* outputMaterial,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (outputMaterial == NULL)
    {
        return TRUE;
    }

    if (nodeMaterial == NULL)
    {
        return TRUE;
    }

    if (nodeMaterial->ClassID() !=
        Class_ID(DMTL_CLASS_ID, 0))
    {
        return TRUE;
    }

    StdMat* standardMaterial =
        static_cast<StdMat*>(
            nodeMaterial);

    outputMaterial->present =
        TRUE;

    outputMaterial->name =
        ConvertTcharToUtf8(
            nodeMaterial->GetName());

    if (outputMaterial->name.empty())
    {
        outputMaterial->name =
            ConvertTcharToUtf8(
                fallbackMaterialName);
    }

    if (outputMaterial->name.empty())
    {
        outputMaterial->name =
            "MaxGLB_Material";
    }

    const Color diffuseColor =
        standardMaterial->GetDiffuse(
            timeValue);

    outputMaterial->baseColor[0] =
        ClampUnitFloat(diffuseColor.r);

    outputMaterial->baseColor[1] =
        ClampUnitFloat(diffuseColor.g);

    outputMaterial->baseColor[2] =
        ClampUnitFloat(diffuseColor.b);

    outputMaterial->baseColor[3] =
        ClampUnitFloat(
            standardMaterial->GetOpacity(
                timeValue));

    outputMaterial->doubleSided =
        standardMaterial->GetTwoSided();

    int storedAlphaMode =
        MAXGLB_ALPHA_OPAQUE;

    float storedAlphaCutoff =
        0.5f;

    DWORD storedAlphaFlags = 0;

    const BOOL hasStoredAlpha =
        ReadMaxGLBAlphaMetadata(
            nodeMaterial,
            &storedAlphaMode,
            &storedAlphaCutoff,
            &storedAlphaFlags);

    outputMaterial->alphaMetadataFlags =
        storedAlphaFlags;

    if (exportSettings.alphaMode !=
        MAXGLB_ALPHA_AUTO)
    {
        outputMaterial->alphaMode =
            exportSettings.alphaMode;

        outputMaterial->alphaCutoff =
            exportSettings.alphaCutoff;
    }
    else if (hasStoredAlpha)
    {
        outputMaterial->alphaMode =
            storedAlphaMode;

        outputMaterial->alphaCutoff =
            storedAlphaCutoff;
    }
    else
    {
        outputMaterial->alphaMode =
            outputMaterial->baseColor[3] <
                0.999f
            ? MAXGLB_ALPHA_BLEND
            : MAXGLB_ALPHA_OPAQUE;

        outputMaterial->alphaCutoff =
            exportSettings.alphaCutoff;
    }

    if (outputMaterial->alphaMode ==
        MAXGLB_ALPHA_OPAQUE)
    {
        outputMaterial->baseColor[3] =
            1.0f;
    }

    if (!exportSettings.exportTextures)
    {
        return TRUE;
    }

    const std::string baseName =
        outputMaterial->name;

    BitmapTex* diffuseBitmap =
        GetDirectBitmapFromStandardSlot(
            nodeMaterial,
            standardMaterial,
            ID_DI);

    BitmapTex* opacityBitmap =
        GetDirectBitmapFromStandardSlot(
            nodeMaterial,
            standardMaterial,
            ID_OP);

    BitmapTex* normalBitmap =
        GetDirectBitmapFromStandardSlot(
            nodeMaterial,
            standardMaterial,
            ID_BU);

    BitmapTex* occlusionBitmap =
        GetDirectBitmapFromStandardSlot(
            nodeMaterial,
            standardMaterial,
            ID_AM);

    BitmapTex* glossinessBitmap =
        GetDirectBitmapFromStandardSlot(
            nodeMaterial,
            standardMaterial,
            ID_SH);

    BitmapTex* metallicBitmap =
        GetDirectBitmapFromStandardSlot(
            nodeMaterial,
            standardMaterial,
            ID_SS);

    BitmapTex* emissiveBitmap =
        GetDirectBitmapFromStandardSlot(
            nodeMaterial,
            standardMaterial,
            ID_SI);

    MaxGLBExportImageData diffuseImage;

    if (!LoadBitmapImage(
            diffuseBitmap,
            &diffuseImage,
            baseName + "_BaseColor",
            errorMessage,
            errorMessageCount))
    {
        return FALSE;
    }

    if (opacityBitmap != NULL &&
        outputMaterial->alphaMode !=
            MAXGLB_ALPHA_OPAQUE)
    {
        MaxGLBExportImageData opacityImage;

        if (!LoadBitmapImage(
                opacityBitmap,
                &opacityImage,
                baseName + "_Opacity",
                errorMessage,
                errorMessageCount))
        {
            return FALSE;
        }

        MaxGLBDecodedImage decodedOpacity;

        if (!DecodeImageWithWic(
                opacityImage.bytes,
                &decodedOpacity,
                errorMessage,
                errorMessageCount))
        {
            return FALSE;
        }

        MaxGLBDecodedImage combinedBaseColor;

        if (diffuseImage.present)
        {
            if (!DecodeImageWithWic(
                    diffuseImage.bytes,
                    &combinedBaseColor,
                    errorMessage,
                    errorMessageCount))
            {
                return FALSE;
            }

            if (combinedBaseColor.width !=
                    decodedOpacity.width ||
                combinedBaseColor.height !=
                    decodedOpacity.height)
            {
                _tcscpy_s(
                    errorMessage,
                    errorMessageCount,
                    _T("The diffuse and opacity bitmaps must have identical dimensions."));
                return FALSE;
            }
        }
        else
        {
            combinedBaseColor.width =
                decodedOpacity.width;

            combinedBaseColor.height =
                decodedOpacity.height;

            combinedBaseColor.rgba.resize(
                static_cast<size_t>(
                    decodedOpacity.width) *
                static_cast<size_t>(
                    decodedOpacity.height) *
                4,
                255);
        }

        const size_t pixelCount =
            static_cast<size_t>(
                combinedBaseColor.width) *
            static_cast<size_t>(
                combinedBaseColor.height);

        for (size_t pixelIndex = 0;
             pixelIndex < pixelCount;
             ++pixelIndex)
        {
            const size_t alphaIndex =
                pixelIndex * 4 + 3;

            const unsigned int existingAlpha =
                combinedBaseColor.rgba[
                    alphaIndex];

            const unsigned int opacity =
                GetGrayValue(
                    decodedOpacity,
                    pixelIndex);

            if ((outputMaterial->alphaMetadataFlags &
                 MAXGLB_ALPHA_FLAG_OPACITY_FROM_BASE_ALPHA) != 0)
            {
                combinedBaseColor.rgba[
                    alphaIndex] =
                        static_cast<unsigned char>(
                            opacity);
            }
            else
            {
                combinedBaseColor.rgba[
                    alphaIndex] =
                        static_cast<unsigned char>(
                            (existingAlpha *
                             opacity) / 255);
            }
        }

        MaxGLBExportImageData& baseOutput =
            outputMaterial->textures[
                MAXGLB_TEXTURE_BASE_COLOR];

        if (!EncodeRgbaPngWithWic(
                combinedBaseColor,
                baseOutput.bytes,
                errorMessage,
                errorMessageCount))
        {
            return FALSE;
        }

        baseOutput.present =
            TRUE;

        baseOutput.name =
            baseName + "_BaseColorOpacity";

        baseOutput.mimeType =
            "image/png";

        if (exportSettings.alphaMode ==
                MAXGLB_ALPHA_AUTO &&
            !hasStoredAlpha)
        {
            outputMaterial->alphaMode =
                MAXGLB_ALPHA_BLEND;
        }
    }
    else if (diffuseImage.present)
    {
        outputMaterial->textures[
            MAXGLB_TEXTURE_BASE_COLOR] =
                diffuseImage;

        MaxGLBDecodedImage decodedBaseColor;

        if (DecodeImageWithWic(
                diffuseImage.bytes,
                &decodedBaseColor,
                errorMessage,
                errorMessageCount))
        {
            const size_t pixelCount =
                static_cast<size_t>(
                    decodedBaseColor.width) *
                static_cast<size_t>(
                    decodedBaseColor.height);

            for (size_t pixelIndex = 0;
                 pixelIndex < pixelCount;
                 ++pixelIndex)
            {
                if (decodedBaseColor.rgba[
                        pixelIndex * 4 + 3] <
                    255)
                {
                    if (exportSettings.alphaMode ==
                            MAXGLB_ALPHA_AUTO &&
                        !hasStoredAlpha)
                    {
                        outputMaterial->alphaMode =
                            MAXGLB_ALPHA_BLEND;
                    }

                    break;
                }
            }
        }
    }

    if (!LoadBitmapImage(
            normalBitmap,
            &outputMaterial->textures[
                MAXGLB_TEXTURE_NORMAL],
            baseName + "_Normal",
            errorMessage,
            errorMessageCount))
    {
        return FALSE;
    }

    if (!LoadBitmapImage(
            emissiveBitmap,
            &outputMaterial->textures[
                MAXGLB_TEXTURE_EMISSIVE],
            baseName + "_Emissive",
            errorMessage,
            errorMessageCount))
    {
        return FALSE;
    }

    if (occlusionBitmap != NULL ||
        glossinessBitmap != NULL ||
        metallicBitmap != NULL)
    {
        MaxGLBExportImageData& ormOutput =
            outputMaterial->textures[
                MAXGLB_TEXTURE_ORM];

        BOOL reusedOriginalOrm = FALSE;

        if (!TryReuseOriginalImportedOrm(
                occlusionBitmap,
                glossinessBitmap,
                metallicBitmap,
                baseName + "_OcclusionRoughnessMetallic",
                &ormOutput,
                &reusedOriginalOrm,
                errorMessage,
                errorMessageCount))
        {
            return FALSE;
        }

        if (reusedOriginalOrm)
        {
            // The importer saved the untouched packed source image beside
            // the three editable legacy maps. Reusing it preserves the
            // exact linear R/G/B values for a lossless GLB -> Max -> GLB
            // material round-trip.
            outputMaterial->preservedOriginalOrm =
                TRUE;
        }
        else
        {
        MaxGLBExportImageData occlusionImage;
        MaxGLBExportImageData glossinessImage;
        MaxGLBExportImageData metallicImage;

        if (!LoadBitmapImage(
                occlusionBitmap,
                &occlusionImage,
                baseName + "_Occlusion",
                errorMessage,
                errorMessageCount) ||
            !LoadBitmapImage(
                glossinessBitmap,
                &glossinessImage,
                baseName + "_Glossiness",
                errorMessage,
                errorMessageCount) ||
            !LoadBitmapImage(
                metallicBitmap,
                &metallicImage,
                baseName + "_Metallic",
                errorMessage,
                errorMessageCount))
        {
            return FALSE;
        }

        MaxGLBDecodedImage decodedOcclusion;
        MaxGLBDecodedImage decodedGlossiness;
        MaxGLBDecodedImage decodedMetallic;

        UINT packedWidth = 0;
        UINT packedHeight = 0;

        if (occlusionImage.present)
        {
            if (!DecodeImageWithWic(
                    occlusionImage.bytes,
                    &decodedOcclusion,
                    errorMessage,
                    errorMessageCount))
            {
                return FALSE;
            }

            packedWidth =
                decodedOcclusion.width;

            packedHeight =
                decodedOcclusion.height;
        }

        if (glossinessImage.present)
        {
            if (!DecodeImageWithWic(
                    glossinessImage.bytes,
                    &decodedGlossiness,
                    errorMessage,
                    errorMessageCount))
            {
                return FALSE;
            }

            if (packedWidth == 0)
            {
                packedWidth =
                    decodedGlossiness.width;

                packedHeight =
                    decodedGlossiness.height;
            }
        }

        if (metallicImage.present)
        {
            if (!DecodeImageWithWic(
                    metallicImage.bytes,
                    &decodedMetallic,
                    errorMessage,
                    errorMessageCount))
            {
                return FALSE;
            }

            if (packedWidth == 0)
            {
                packedWidth =
                    decodedMetallic.width;

                packedHeight =
                    decodedMetallic.height;
            }
        }

        if ((decodedOcclusion.width != 0 &&
             (decodedOcclusion.width != packedWidth ||
              decodedOcclusion.height != packedHeight)) ||
            (decodedGlossiness.width != 0 &&
             (decodedGlossiness.width != packedWidth ||
              decodedGlossiness.height != packedHeight)) ||
            (decodedMetallic.width != 0 &&
             (decodedMetallic.width != packedWidth ||
              decodedMetallic.height != packedHeight)))
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("Occlusion, glossiness and metallic bitmaps must have identical dimensions."));
            return FALSE;
        }

        MaxGLBDecodedImage packedOrm;

        packedOrm.width =
            packedWidth;

        packedOrm.height =
            packedHeight;

        const size_t pixelCount =
            static_cast<size_t>(packedWidth) *
            static_cast<size_t>(packedHeight);

        packedOrm.rgba.resize(
            pixelCount * 4);

        for (size_t pixelIndex = 0;
             pixelIndex < pixelCount;
             ++pixelIndex)
        {
            const unsigned char occlusion =
                decodedOcclusion.width != 0
                ? GetGrayValue(
                    decodedOcclusion,
                    pixelIndex)
                : 255;

            const unsigned char glossiness =
                decodedGlossiness.width != 0
                ? GetGrayValue(
                    decodedGlossiness,
                    pixelIndex)
                : 0;

            const unsigned char metallic =
                decodedMetallic.width != 0
                ? GetGrayValue(
                    decodedMetallic,
                    pixelIndex)
                : 0;

            const size_t rgbaIndex =
                pixelIndex * 4;

            packedOrm.rgba[rgbaIndex + 0] =
                occlusion;

            // Max Standard uses glossiness.
            // glTF uses roughness.
            packedOrm.rgba[rgbaIndex + 1] =
                static_cast<unsigned char>(
                    255 - glossiness);

            packedOrm.rgba[rgbaIndex + 2] =
                metallic;

            packedOrm.rgba[rgbaIndex + 3] =
                255;
        }

        if (!EncodeRgbaPngWithWic(
                packedOrm,
                ormOutput.bytes,
                errorMessage,
                errorMessageCount))
        {
            return FALSE;
        }

        ormOutput.present =
            TRUE;

        ormOutput.name =
            baseName + "_OcclusionRoughnessMetallic";

        ormOutput.mimeType =
            "image/png";
        }
    }

    return TRUE;
}


static Mtl* ResolveFaceMaterial(
    Mtl* rootMaterial,
    int materialId)
{
    if (rootMaterial == NULL)
    {
        return NULL;
    }

    if (rootMaterial->IsMultiMtl())
    {
        const int subMaterialCount =
            rootMaterial->NumSubMtls();

        if (materialId >= 0 &&
            materialId < subMaterialCount)
        {
            Mtl* subMaterial =
                rootMaterial->GetSubMtl(
                    materialId);

            if (subMaterial != NULL)
            {
                return subMaterial;
            }
        }

        return NULL;
    }

    return rootMaterial;
}


static BOOL TryBuildExportMeshes(
    INode* node,
    TimeValue timeValue,
    const MaxGLBExportSettings& settings,
    int nodeGroupIndex,
    std::vector<MaxGLBExportMesh>& outputMeshes,
    BOOL* outWasGeometry,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (outWasGeometry != NULL)
    {
        *outWasGeometry = FALSE;
    }

    if (node == NULL)
    {
        return TRUE;
    }

    TCHAR exportProgressStep[384];

    _stprintf_s(
        exportProgressStep,
        _countof(exportProgressStep),
        _T("Processing %s (%d of %d)"),
        node->GetName(),
        g_exportProgressProcessedNodes + 1,
        g_exportProgressTotalNodes);

    const int exportNodePercent =
        2 +
        (g_exportProgressProcessedNodes * 68) /
        (g_exportProgressTotalNodes > 0
            ? g_exportProgressTotalNodes
            : 1);

    if (!UpdateMaxGLBProgress(
            exportNodePercent,
            exportProgressStep))
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Export cancelled by the user."));
        return FALSE;
    }

    const ObjectState& objectState =
        node->EvalWorldState(timeValue);

    Object* evaluatedObject =
        objectState.obj;

    if (evaluatedObject == NULL ||
        !evaluatedObject->CanConvertToType(
            triObjectClassID))
    {
        return TRUE;
    }

    TriObject* triangleObject =
        static_cast<TriObject*>(
            evaluatedObject->ConvertToType(
                timeValue,
                triObjectClassID));

    if (triangleObject == NULL)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("A scene object could not be converted to an Editable Mesh."));
        return FALSE;
    }

    const BOOL deleteConvertedObject =
        triangleObject != evaluatedObject;

    Mesh& mesh =
        triangleObject->GetMesh();

    const int sourceVertexCount =
        mesh.getNumVerts();

    const int faceCount =
        mesh.getNumFaces();

    if (sourceVertexCount <= 0 ||
        faceCount <= 0)
    {
        if (deleteConvertedObject)
        {
            triangleObject->DeleteThis();
        }

        return TRUE;
    }

    std::string nodeName =
        ConvertTcharToUtf8(
            node->GetName());

    if (nodeName.empty())
    {
        nodeName =
            "MaxGLB_Mesh";
    }

    Mtl* rootMaterial =
        node->GetMtl();

    const BOOL usesMultiMaterial =
        rootMaterial != NULL &&
        rootMaterial->IsMultiMtl() &&
        rootMaterial->NumSubMtls() > 0;

    std::set<int> usedMaterialIds;

    if (usesMultiMaterial)
    {
        for (int faceIndex = 0;
             faceIndex < faceCount;
             ++faceIndex)
        {
            usedMaterialIds.insert(
                static_cast<int>(
                    mesh.faces[faceIndex]
                        .getMatID()));
        }
    }
    else
    {
        usedMaterialIds.insert(0);
    }

    const Matrix3 objectTransform =
        node->GetObjectTM(timeValue);

    const BOOL reverseWinding =
        GetMatrixDeterminant3x3(
            objectTransform) < 0.0f;

    const BOOL hasTexcoords =
        mesh.getNumTVerts() > 0 &&
        mesh.tVerts != NULL &&
        mesh.tvFace != NULL;

    const int textureVertexCount =
        mesh.getNumTVerts();

    mesh.SpecifyNormals();

    MeshNormalSpec* normalSpec =
        mesh.GetSpecifiedNormals();

    if (normalSpec != NULL)
    {
        normalSpec->SetParent(&mesh);
        normalSpec->CheckNormals();
    }

    int primitiveNumber = 0;

    for (std::set<int>::const_iterator materialIdIterator =
            usedMaterialIds.begin();
         materialIdIterator != usedMaterialIds.end();
         ++materialIdIterator)
    {
        if (!ContinueMaxGLBOperation(
                errorMessage,
                errorMessageCount))
        {
            if (deleteConvertedObject)
            {
                triangleObject->DeleteThis();
            }

            return FALSE;
        }

        const int materialId =
            *materialIdIterator;

        MaxGLBExportMesh outputMesh;

        outputMesh.nodeGroupIndex =
            nodeGroupIndex;

        outputMesh.sourceMaterialId =
            materialId;

        outputMesh.nodeName =
            nodeName;

        outputMesh.name =
            nodeName;

        if (usedMaterialIds.size() > 1)
        {
            std::ostringstream primitiveName;
            primitiveName
                << nodeName
                << "_MaterialID_"
                << (materialId + 1);

            outputMesh.name =
                primitiveName.str();
        }

        outputMesh.hasTexcoords =
            hasTexcoords;

        Mtl* faceMaterial =
            ResolveFaceMaterial(
                rootMaterial,
                materialId);

        if (settings.exportMaterials &&
            !CaptureMaterialMaps(
                faceMaterial,
                node->GetName(),
                timeValue,
                settings,
                &outputMesh.material,
                errorMessage,
                errorMessageCount))
        {
            if (deleteConvertedObject)
            {
                triangleObject->DeleteThis();
            }

            return FALSE;
        }

        size_t matchingFaceCount = 0;

        for (int faceIndex = 0;
             faceIndex < faceCount;
             ++faceIndex)
        {
            if (!usesMultiMaterial ||
                static_cast<int>(
                    mesh.faces[faceIndex]
                        .getMatID()) == materialId)
            {
                ++matchingFaceCount;
            }
        }

        if (matchingFaceCount == 0)
        {
            continue;
        }

        const size_t maximumOutputVertexCount =
            matchingFaceCount * 3;

        outputMesh.positions.reserve(
            maximumOutputVertexCount * 3);

        outputMesh.normals.reserve(
            maximumOutputVertexCount * 3);

        if (outputMesh.hasTexcoords)
        {
            outputMesh.texcoords.reserve(
                maximumOutputVertexCount * 2);
        }

        outputMesh.indices.reserve(
            maximumOutputVertexCount);

        BOOL boundsInitialized = FALSE;

        std::map<
            MaxGLBExportVertexKey,
            unsigned int> exportedVertexLookup;

        for (int faceIndex = 0;
             faceIndex < faceCount;
             ++faceIndex)
        {
            if (usesMultiMaterial &&
                static_cast<int>(
                    mesh.faces[faceIndex]
                        .getMatID()) != materialId)
            {
                continue;
            }

            if ((faceIndex & 4095) == 0 &&
                !ContinueMaxGLBOperation(
                    errorMessage,
                    errorMessageCount))
            {
                if (deleteConvertedObject)
                {
                    triangleObject->DeleteThis();
                }

                return FALSE;
            }

            const Face& face =
                mesh.faces[faceIndex];

            int cornerOrder[3] =
            {
                0,
                1,
                2
            };

            if (reverseWinding)
            {
                cornerOrder[1] = 2;
                cornerOrder[2] = 1;
            }

            Point3 fallbackPositions[3];

            for (int sourceCorner = 0;
                 sourceCorner < 3;
                 ++sourceCorner)
            {
                const int sourceVertexIndex =
                    face.v[sourceCorner];

                if (sourceVertexIndex < 0 ||
                    sourceVertexIndex >=
                        sourceVertexCount)
                {
                    if (deleteConvertedObject)
                    {
                        triangleObject->DeleteThis();
                    }

                    _tcscpy_s(
                        errorMessage,
                        errorMessageCount,
                        _T("A mesh contains an invalid face vertex index."));
                    return FALSE;
                }

                fallbackPositions[sourceCorner] =
                    ConvertMaxWorldPositionToGltf(
                        mesh.getVert(
                            sourceVertexIndex) *
                        objectTransform);
            }

            Point3 fallbackFaceNormal =
                CrossProd(
                    fallbackPositions[1] -
                        fallbackPositions[0],
                    fallbackPositions[2] -
                        fallbackPositions[0]);

            if (reverseWinding)
            {
                fallbackFaceNormal =
                    -fallbackFaceNormal;
            }

            const float fallbackLengthSquared =
                DotProd(
                    fallbackFaceNormal,
                    fallbackFaceNormal);

            if (fallbackLengthSquared > 1.0e-20f)
            {
                fallbackFaceNormal /=
                    sqrtf(
                        fallbackLengthSquared);
            }
            else
            {
                fallbackFaceNormal =
                    Point3(
                        0.0f,
                        1.0f,
                        0.0f);
            }

            for (int outputCorner = 0;
                 outputCorner < 3;
                 ++outputCorner)
            {
                const int sourceCorner =
                    cornerOrder[outputCorner];

                const int sourceVertexIndex =
                    face.v[sourceCorner];

                const Point3 gltfPosition =
                    fallbackPositions[
                        sourceCorner];

                Point3 gltfNormal =
                    fallbackFaceNormal;

                if (normalSpec != NULL &&
                    normalSpec->GetNumFaces() ==
                        faceCount)
                {
                    const Point3 localNormal =
                        normalSpec->GetNormal(
                            faceIndex,
                            sourceCorner);

                    const Point3 worldNormal =
                        TransformNormalToWorld(
                            localNormal,
                            objectTransform);

                    gltfNormal =
                        ConvertMaxWorldNormalToGltf(
                            worldNormal);
                }

                float gltfU = 0.0f;
                float gltfV = 0.0f;

                if (outputMesh.hasTexcoords)
                {
                    const int textureVertexIndex =
                        mesh.tvFace[faceIndex]
                            .t[sourceCorner];

                    if (textureVertexIndex < 0 ||
                        textureVertexIndex >=
                            textureVertexCount)
                    {
                        if (deleteConvertedObject)
                        {
                            triangleObject->DeleteThis();
                        }

                        _tcscpy_s(
                            errorMessage,
                            errorMessageCount,
                            _T("A mesh contains an invalid channel-1 UV index."));
                        return FALSE;
                    }

                    const UVVert& maxUv =
                        mesh.tVerts[
                            textureVertexIndex];

                    gltfU =
                        maxUv.x;

                    gltfV =
                        1.0f - maxUv.y;
                }

                const MaxGLBExportVertexKey vertexKey(
                    gltfPosition,
                    gltfNormal,
                    outputMesh.hasTexcoords,
                    gltfU,
                    gltfV);

                std::map<
                    MaxGLBExportVertexKey,
                    unsigned int>::iterator existingVertex =
                        exportedVertexLookup.find(
                            vertexKey);

                unsigned int exportVertexIndex;

                if (existingVertex !=
                    exportedVertexLookup.end())
                {
                    exportVertexIndex =
                        existingVertex->second;
                }
                else
                {
                    exportVertexIndex =
                        static_cast<unsigned int>(
                            outputMesh.positions.size() / 3);

                    outputMesh.positions.push_back(
                        gltfPosition.x);

                    outputMesh.positions.push_back(
                        gltfPosition.y);

                    outputMesh.positions.push_back(
                        gltfPosition.z);

                    outputMesh.normals.push_back(
                        gltfNormal.x);

                    outputMesh.normals.push_back(
                        gltfNormal.y);

                    outputMesh.normals.push_back(
                        gltfNormal.z);

                    if (outputMesh.hasTexcoords)
                    {
                        outputMesh.texcoords.push_back(
                            gltfU);

                        outputMesh.texcoords.push_back(
                            gltfV);
                    }

                    exportedVertexLookup.insert(
                        std::make_pair(
                            vertexKey,
                            exportVertexIndex));

                    if (!boundsInitialized)
                    {
                        outputMesh.minimum[0] =
                            outputMesh.maximum[0] =
                                gltfPosition.x;

                        outputMesh.minimum[1] =
                            outputMesh.maximum[1] =
                                gltfPosition.y;

                        outputMesh.minimum[2] =
                            outputMesh.maximum[2] =
                                gltfPosition.z;

                        boundsInitialized =
                            TRUE;
                    }
                    else
                    {
                        const float positionValues[3] =
                        {
                            gltfPosition.x,
                            gltfPosition.y,
                            gltfPosition.z
                        };

                        for (int axisIndex = 0;
                             axisIndex < 3;
                             ++axisIndex)
                        {
                            if (positionValues[axisIndex] <
                                outputMesh.minimum[axisIndex])
                            {
                                outputMesh.minimum[axisIndex] =
                                    positionValues[axisIndex];
                            }

                            if (positionValues[axisIndex] >
                                outputMesh.maximum[axisIndex])
                            {
                                outputMesh.maximum[axisIndex] =
                                    positionValues[axisIndex];
                            }
                        }
                    }
                }

                outputMesh.indices.push_back(
                    exportVertexIndex);
            }
        }

        if (!outputMesh.indices.empty())
        {
            outputMeshes.push_back(
                outputMesh);

            ++primitiveNumber;
        }
    }

    if (deleteConvertedObject)
    {
        triangleObject->DeleteThis();
    }

    if (outWasGeometry != NULL)
    {
        *outWasGeometry =
            primitiveNumber > 0;
    }

    return TRUE;
}


static BOOL CollectSceneMeshesRecursive(
    INode* parentNode,
    TimeValue timeValue,
    const MaxGLBExportSettings& settings,
    std::vector<MaxGLBExportMesh>& outputMeshes,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (parentNode == NULL)
    {
        return TRUE;
    }

    const int childCount =
        parentNode->NumberOfChildren();

    for (int childIndex = 0;
         childIndex < childCount;
         ++childIndex)
    {
        INode* childNode =
            parentNode->GetChildNode(
                childIndex);

        if (childNode == NULL)
        {
            continue;
        }

        BOOL wasGeometry = FALSE;

        if (!TryBuildExportMeshes(
                childNode,
                timeValue,
                settings,
                g_exportNextNodeGroupIndex,
                outputMeshes,
                &wasGeometry,
                errorMessage,
                errorMessageCount))
        {
            return FALSE;
        }

        ++g_exportProgressProcessedNodes;

        if (wasGeometry)
        {
            ++g_exportNextNodeGroupIndex;
        }

        if (!CollectSceneMeshesRecursive(
                childNode,
                timeValue,
                settings,
                outputMeshes,
                errorMessage,
                errorMessageCount))
        {
            return FALSE;
        }
    }

    return TRUE;
}


static BOOL CollectMeshesForExport(
    Interface* maxInterface,
    BOOL selectedOnly,
    const MaxGLBExportSettings& settings,
    std::vector<MaxGLBExportMesh>& outputMeshes,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (maxInterface == NULL)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The 3ds Max interface is unavailable."));
        return FALSE;
    }

    const TimeValue timeValue =
        maxInterface->GetTime();

    if (selectedOnly)
    {
        const int selectedNodeCount =
            maxInterface->GetSelNodeCount();

        if (selectedNodeCount <= 0)
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("Export Selected was requested, but no objects are selected."));
            return FALSE;
        }

        for (int selectedNodeIndex = 0;
             selectedNodeIndex < selectedNodeCount;
             ++selectedNodeIndex)
        {
            INode* selectedNode =
                maxInterface->GetSelNode(
                    selectedNodeIndex);

            BOOL wasGeometry = FALSE;

            if (!TryBuildExportMeshes(
                    selectedNode,
                    timeValue,
                    settings,
                    g_exportNextNodeGroupIndex,
                    outputMeshes,
                    &wasGeometry,
                    errorMessage,
                    errorMessageCount))
            {
                return FALSE;
            }

            ++g_exportProgressProcessedNodes;

            if (wasGeometry)
            {
                ++g_exportNextNodeGroupIndex;
            }
        }
    }
    else
    {
        if (!CollectSceneMeshesRecursive(
                maxInterface->GetRootNode(),
                timeValue,
                settings,
                outputMeshes,
                errorMessage,
                errorMessageCount))
        {
            return FALSE;
        }
    }

    if (outputMeshes.empty())
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            selectedOnly
                ? _T("The current selection contains no exportable geometry.")
                : _T("The scene contains no exportable geometry."));
        return FALSE;
    }

    return TRUE;
}



static BOOL ApplyExportSizeSettings(
    std::vector<MaxGLBExportMesh>& meshes,
    MaxGLBExportSettings* settings,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (!UpdateMaxGLBProgress(
            72,
            _T("Applying export size settings...")))
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Export cancelled by the user."));
        return FALSE;
    }

    if (settings == NULL ||
        meshes.empty())
    {
        return TRUE;
    }

    float uniformScale =
        settings->scaleFactor;

    if (settings->normalizeSize)
    {
        BOOL boundsInitialized = FALSE;

        float globalMinimum[3] =
        {
            0.0f,
            0.0f,
            0.0f
        };

        float globalMaximum[3] =
        {
            0.0f,
            0.0f,
            0.0f
        };

        for (size_t meshIndex = 0;
             meshIndex < meshes.size();
             ++meshIndex)
        {
            const MaxGLBExportMesh& mesh =
                meshes[meshIndex];

            if (!boundsInitialized)
            {
                for (int axisIndex = 0;
                     axisIndex < 3;
                     ++axisIndex)
                {
                    globalMinimum[axisIndex] =
                        mesh.minimum[axisIndex];

                    globalMaximum[axisIndex] =
                        mesh.maximum[axisIndex];
                }

                boundsInitialized = TRUE;
            }
            else
            {
                for (int axisIndex = 0;
                     axisIndex < 3;
                     ++axisIndex)
                {
                    if (mesh.minimum[axisIndex] <
                        globalMinimum[axisIndex])
                    {
                        globalMinimum[axisIndex] =
                            mesh.minimum[axisIndex];
                    }

                    if (mesh.maximum[axisIndex] >
                        globalMaximum[axisIndex])
                    {
                        globalMaximum[axisIndex] =
                            mesh.maximum[axisIndex];
                    }
                }
            }
        }

        float largestDimension = 0.0f;

        for (int axisIndex = 0;
             axisIndex < 3;
             ++axisIndex)
        {
            const float dimension =
                globalMaximum[axisIndex] -
                globalMinimum[axisIndex];

            if (dimension > largestDimension)
            {
                largestDimension =
                    dimension;
            }
        }

        if (largestDimension <= 1.0e-20f)
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("The export bounds are too small to normalize."));
            return FALSE;
        }

        uniformScale =
            settings->normalizeTargetSize /
            largestDimension;
    }

    if (!_finite(uniformScale) ||
        uniformScale <= 0.0f)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The calculated export scale is invalid."));
        return FALSE;
    }

    for (size_t meshIndex = 0;
         meshIndex < meshes.size();
         ++meshIndex)
    {
        if (!ContinueMaxGLBOperation(
                errorMessage,
                errorMessageCount))
        {
            return FALSE;
        }

        MaxGLBExportMesh& mesh =
            meshes[meshIndex];

        for (size_t positionIndex = 0;
             positionIndex < mesh.positions.size();
             ++positionIndex)
        {
            mesh.positions[positionIndex] *=
                uniformScale;
        }

        for (int axisIndex = 0;
             axisIndex < 3;
             ++axisIndex)
        {
            mesh.minimum[axisIndex] *=
                uniformScale;

            mesh.maximum[axisIndex] *=
                uniformScale;
        }
    }

    settings->appliedUniformScale =
        uniformScale;

    return TRUE;
}


static void ShowExportedFileInExplorer(
    const TCHAR* outputFilename)
{
    if (outputFilename == NULL ||
        outputFilename[0] == _T('\0'))
    {
        return;
    }

    TCHAR parameters[4096];

    if (_stprintf_s(
            parameters,
            _countof(parameters),
            _T("/select,\"%s\""),
            outputFilename) < 0)
    {
        return;
    }

    ShellExecute(
        NULL,
        _T("open"),
        _T("explorer.exe"),
        parameters,
        NULL,
        SW_SHOWNORMAL);
}


static BOOL WriteCancelableBlock(
    FILE* outputFile,
    const void* sourceData,
    size_t byteCount,
    int startPercent,
    int endPercent,
    const TCHAR* stepName,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (byteCount == 0)
    {
        return TRUE;
    }

    if (outputFile == NULL ||
        sourceData == NULL)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Invalid output data was supplied while writing the GLB."));
        return FALSE;
    }

    const unsigned char* sourceBytes =
        reinterpret_cast<const unsigned char*>(
            sourceData);

    const size_t chunkSize =
        1024 * 1024;

    size_t writtenTotal = 0;

    while (writtenTotal < byteCount)
    {
        if (!ContinueMaxGLBOperation(
                errorMessage,
                errorMessageCount))
        {
            return FALSE;
        }

        const size_t remaining =
            byteCount - writtenTotal;

        const size_t thisChunk =
            remaining < chunkSize
                ? remaining
                : chunkSize;

        if (fwrite(
                sourceBytes + writtenTotal,
                1,
                thisChunk,
                outputFile) != thisChunk)
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("The GLB file could not be written completely."));
            return FALSE;
        }

        writtenTotal +=
            thisChunk;

        const int percent =
            startPercent +
            static_cast<int>(
                (writtenTotal *
                 static_cast<size_t>(
                    endPercent - startPercent)) /
                byteCount);

        if (!UpdateMaxGLBProgress(
                percent,
                stepName))
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("Export cancelled by the user."));
            return FALSE;
        }
    }

    return TRUE;
}


static BOOL BuildTemporaryExportFilename(
    const TCHAR* outputFilename,
    TCHAR* temporaryFilename,
    size_t temporaryFilenameCount)
{
    if (outputFilename == NULL ||
        outputFilename[0] == _T('\0') ||
        temporaryFilename == NULL ||
        temporaryFilenameCount == 0)
    {
        return FALSE;
    }

    return _stprintf_s(
        temporaryFilename,
        temporaryFilenameCount,
        _T("%s.MaxGLB2016.tmp"),
        outputFilename) >= 0;
}


static BOOL BuildAlternateExportFilename(
    const TCHAR* requestedFilename,
    TCHAR* alternateFilename,
    size_t alternateFilenameCount)
{
    if (requestedFilename == NULL ||
        requestedFilename[0] == _T('\0') ||
        alternateFilename == NULL ||
        alternateFilenameCount == 0)
    {
        return FALSE;
    }

    const TCHAR* lastBackslash =
        _tcsrchr(
            requestedFilename,
            _T('\\'));

    const TCHAR* lastForwardSlash =
        _tcsrchr(
            requestedFilename,
            _T('/'));

    const TCHAR* lastSlash =
        lastBackslash;

    if (lastForwardSlash != NULL &&
        (lastSlash == NULL ||
         lastForwardSlash > lastSlash))
    {
        lastSlash =
            lastForwardSlash;
    }

    const TCHAR* extension =
        _tcsrchr(
            requestedFilename,
            _T('.'));

    if (extension == NULL ||
        (lastSlash != NULL &&
         extension < lastSlash))
    {
        extension =
            requestedFilename +
            _tcslen(requestedFilename);
    }

    const size_t prefixLength =
        static_cast<size_t>(
            extension - requestedFilename);

    for (int candidateIndex = 1;
         candidateIndex <= 999;
         ++candidateIndex)
    {
        const int written =
            candidateIndex == 1
                ? _stprintf_s(
                    alternateFilename,
                    alternateFilenameCount,
                    _T("%.*s_new%s"),
                    static_cast<int>(
                        prefixLength),
                    requestedFilename,
                    extension)
                : _stprintf_s(
                    alternateFilename,
                    alternateFilenameCount,
                    _T("%.*s_new_%d%s"),
                    static_cast<int>(
                        prefixLength),
                    requestedFilename,
                    candidateIndex,
                    extension);

        if (written < 0)
        {
            return FALSE;
        }

        if (GetFileAttributes(
                alternateFilename) ==
            INVALID_FILE_ATTRIBUTES)
        {
            return TRUE;
        }
    }

    return FALSE;
}


static BOOL CommitTemporaryExportFile(
    const TCHAR* temporaryFilename,
    const TCHAR* requestedOutputFilename,
    TCHAR* actualOutputFilename,
    size_t actualOutputFilenameCount,
    BOOL* usedAlternateFilename,
    DWORD* replacementError,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (actualOutputFilename == NULL ||
        actualOutputFilenameCount == 0)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The final export filename buffer is invalid."));
        return FALSE;
    }

    _tcscpy_s(
        actualOutputFilename,
        actualOutputFilenameCount,
        requestedOutputFilename);

    if (usedAlternateFilename != NULL)
    {
        *usedAlternateFilename =
            FALSE;
    }

    if (replacementError != NULL)
    {
        *replacementError =
            ERROR_SUCCESS;
    }

    DWORD originalAttributes =
        GetFileAttributes(
            requestedOutputFilename);

    const BOOL outputAlreadyExists =
        originalAttributes !=
            INVALID_FILE_ATTRIBUTES;

    const BOOL outputWasReadOnly =
        outputAlreadyExists &&
        (originalAttributes &
         FILE_ATTRIBUTE_READONLY) != 0;

    if (outputWasReadOnly)
    {
        SetFileAttributes(
            requestedOutputFilename,
            originalAttributes &
                ~FILE_ATTRIBUTE_READONLY);
    }

    BOOL committed = FALSE;

    if (outputAlreadyExists)
    {
        committed =
            ReplaceFile(
                requestedOutputFilename,
                temporaryFilename,
                NULL,
                REPLACEFILE_WRITE_THROUGH,
                NULL,
                NULL);
    }
    else
    {
        committed =
            MoveFileEx(
                temporaryFilename,
                requestedOutputFilename,
                MOVEFILE_WRITE_THROUGH);
    }

    if (committed)
    {
        if (outputWasReadOnly)
        {
            SetFileAttributes(
                requestedOutputFilename,
                originalAttributes);
        }

        return TRUE;
    }

    const DWORD win32Error =
        GetLastError();

    if (replacementError != NULL)
    {
        *replacementError =
            win32Error;
    }

    if (outputWasReadOnly)
    {
        SetFileAttributes(
            requestedOutputFilename,
            originalAttributes);
    }

    // Viewers often keep the current GLB open without FILE_SHARE_DELETE.
    // Windows then refuses an atomic replacement with error 5 or 32.
    // Preserve the completed export under a unique sibling filename.
    if (win32Error == ERROR_ACCESS_DENIED ||
        win32Error == ERROR_SHARING_VIOLATION ||
        win32Error == ERROR_LOCK_VIOLATION)
    {
        TCHAR alternateFilename[2048];
        alternateFilename[0] =
            _T('\0');

        if (BuildAlternateExportFilename(
                requestedOutputFilename,
                alternateFilename,
                _countof(alternateFilename)) &&
            MoveFileEx(
                temporaryFilename,
                alternateFilename,
                MOVEFILE_WRITE_THROUGH))
        {
            _tcscpy_s(
                actualOutputFilename,
                actualOutputFilenameCount,
                alternateFilename);

            if (usedAlternateFilename != NULL)
            {
                *usedAlternateFilename =
                    TRUE;
            }

            return TRUE;
        }
    }

    _stprintf_s(
        errorMessage,
        errorMessageCount,
        _T("The temporary GLB was written, but Windows could not finalize the output file.\n\n")
        _T("Windows error: %lu\n\n")
        _T("The destination may be open in another program, read-only, or protected."),
        static_cast<unsigned long>(
            win32Error));

    return FALSE;
}


static size_t CountExportNodeGroups(
    const std::vector<MaxGLBExportMesh>& primitives)
{
    size_t groupCount = 0;

    for (size_t primitiveIndex = 0;
         primitiveIndex < primitives.size();
         ++primitiveIndex)
    {
        const int groupIndex =
            primitives[primitiveIndex]
                .nodeGroupIndex;

        if (groupIndex >= 0 &&
            static_cast<size_t>(
                groupIndex + 1) > groupCount)
        {
            groupCount =
                static_cast<size_t>(
                    groupIndex + 1);
        }
    }

    return groupCount;
}


static const MaxGLBExportMesh* FindFirstExportPrimitiveForGroup(
    const std::vector<MaxGLBExportMesh>& primitives,
    int groupIndex)
{
    for (size_t primitiveIndex = 0;
         primitiveIndex < primitives.size();
         ++primitiveIndex)
    {
        if (primitives[primitiveIndex]
                .nodeGroupIndex == groupIndex)
        {
            return &primitives[primitiveIndex];
        }
    }

    return NULL;
}


static size_t CountPrimitivesForGroup(
    const std::vector<MaxGLBExportMesh>& primitives,
    int groupIndex)
{
    size_t primitiveCount = 0;

    for (size_t primitiveIndex = 0;
         primitiveIndex < primitives.size();
         ++primitiveIndex)
    {
        if (primitives[primitiveIndex]
                .nodeGroupIndex == groupIndex)
        {
            ++primitiveCount;
        }
    }

    return primitiveCount;
}


static BOOL WriteGeometryGlbFile(
    const TCHAR* outputFilename,
    const std::vector<MaxGLBExportMesh>& meshes,
    BOOL selectedOnly,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (!UpdateMaxGLBProgress(
            76,
            _T("Packing GLB buffers and textures...")))
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("Export cancelled by the user."));
        return FALSE;
    }

    if (outputFilename == NULL ||
        outputFilename[0] == _T('\0'))
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("No GLB output filename was provided."));
        return FALSE;
    }

    if (meshes.empty())
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("There is no geometry to write."));
        return FALSE;
    }

    std::vector<unsigned char> binaryData;
    std::vector<MaxGLBExportLayout> layouts;

    layouts.resize(meshes.size());

    size_t nextBufferViewIndex = 0;
    size_t nextAccessorIndex = 0;
    size_t nextMaterialIndex = 0;
    size_t nextTextureIndex = 0;
    size_t nextImageIndex = 0;

    for (size_t meshIndex = 0;
         meshIndex < meshes.size();
         ++meshIndex)
    {
        if (!ContinueMaxGLBOperation(
                errorMessage,
                errorMessageCount))
        {
            return FALSE;
        }

        const MaxGLBExportMesh& mesh =
            meshes[meshIndex];

        MaxGLBExportLayout& layout =
            layouts[meshIndex];

        PadBinaryToFourBytes(binaryData);

        layout.positionOffset =
            binaryData.size();

        layout.positionLength =
            mesh.positions.size() *
            sizeof(float);

        layout.positionBufferView =
            nextBufferViewIndex++;

        layout.positionAccessor =
            nextAccessorIndex++;

        AppendBinaryBytes(
            binaryData,
            mesh.positions.empty()
                ? NULL
                : &mesh.positions[0],
            layout.positionLength);

        PadBinaryToFourBytes(binaryData);

        layout.normalOffset =
            binaryData.size();

        layout.normalLength =
            mesh.normals.size() *
            sizeof(float);

        layout.normalBufferView =
            nextBufferViewIndex++;

        layout.normalAccessor =
            nextAccessorIndex++;

        AppendBinaryBytes(
            binaryData,
            mesh.normals.empty()
                ? NULL
                : &mesh.normals[0],
            layout.normalLength);

        if (mesh.hasTexcoords)
        {
            PadBinaryToFourBytes(binaryData);

            layout.texcoordOffset =
                binaryData.size();

            layout.texcoordLength =
                mesh.texcoords.size() *
                sizeof(float);

            layout.texcoordBufferView =
                nextBufferViewIndex++;

            layout.texcoordAccessor =
                nextAccessorIndex++;

            AppendBinaryBytes(
                binaryData,
                mesh.texcoords.empty()
                    ? NULL
                    : &mesh.texcoords[0],
                layout.texcoordLength);
        }

        PadBinaryToFourBytes(binaryData);

        layout.indexOffset =
            binaryData.size();

        layout.indexLength =
            mesh.indices.size() *
            sizeof(unsigned int);

        layout.indexBufferView =
            nextBufferViewIndex++;

        layout.indexAccessor =
            nextAccessorIndex++;

        AppendBinaryBytes(
            binaryData,
            mesh.indices.empty()
                ? NULL
                : &mesh.indices[0],
            layout.indexLength);

        layout.hasMaterial =
            mesh.material.present;

        if (layout.hasMaterial)
        {
            layout.materialIndex =
                nextMaterialIndex++;

            for (int roleIndex = 0;
                 roleIndex < MAXGLB_TEXTURE_COUNT;
                 ++roleIndex)
            {
                const MaxGLBExportImageData& image =
                    mesh.material.textures[
                        roleIndex];

                layout.hasTexture[roleIndex] =
                    image.present;

                if (!image.present)
                {
                    continue;
                }

                PadBinaryToFourBytes(binaryData);

                layout.imageOffset[roleIndex] =
                    binaryData.size();

                layout.imageLength[roleIndex] =
                    image.bytes.size();

                layout.imageBufferView[roleIndex] =
                    nextBufferViewIndex++;

                layout.textureIndex[roleIndex] =
                    nextTextureIndex++;

                layout.imageIndex[roleIndex] =
                    nextImageIndex++;

                AppendBinaryBytes(
                    binaryData,
                    &image.bytes[0],
                    image.bytes.size());
            }
        }
    }

    const size_t unpaddedBinaryLength =
        binaryData.size();

    PadBinaryToFourBytes(binaryData);

    const size_t nodeGroupCount =
        CountExportNodeGroups(meshes);

    std::ostringstream json;
    json.imbue(std::locale::classic());
    json << std::setprecision(9);

    json
        << "{"
        << "\"asset\":{"
        << "\"version\":\"2.0\","
        << "\"generator\":\"MaxGLB2016\""
        << "},"
        << "\"scene\":0,"
        << "\"scenes\":[{\"nodes\":[";

    for (size_t nodeIndex = 0;
         nodeIndex < nodeGroupCount;
         ++nodeIndex)
    {
        if (nodeIndex > 0)
        {
            json << ",";
        }

        json << nodeIndex;
    }

    json
        << "]}],"
        << "\"nodes\":[";

    for (size_t nodeIndex = 0;
         nodeIndex < nodeGroupCount;
         ++nodeIndex)
    {
        if (nodeIndex > 0)
        {
            json << ",";
        }

        const MaxGLBExportMesh* firstPrimitive =
            FindFirstExportPrimitiveForGroup(
                meshes,
                static_cast<int>(
                    nodeIndex));

        json
            << "{"
            << "\"mesh\":"
            << nodeIndex
            << ",\"name\":\""
            << EscapeJsonString(
                firstPrimitive != NULL
                    ? firstPrimitive->nodeName
                    : std::string("MaxGLB_Mesh"))
            << "\""
            << "}";
    }

    json
        << "],"
        << "\"meshes\":[";

    for (size_t groupIndex = 0;
         groupIndex < nodeGroupCount;
         ++groupIndex)
    {
        if (groupIndex > 0)
        {
            json << ",";
        }

        const MaxGLBExportMesh* firstPrimitive =
            FindFirstExportPrimitiveForGroup(
                meshes,
                static_cast<int>(
                    groupIndex));

        json
            << "{"
            << "\"name\":\""
            << EscapeJsonString(
                firstPrimitive != NULL
                    ? firstPrimitive->nodeName
                    : std::string("MaxGLB_Mesh"))
            << "\","
            << "\"primitives\":[";

        BOOL firstPrimitiveJson = TRUE;

        for (size_t primitiveIndex = 0;
             primitiveIndex < meshes.size();
             ++primitiveIndex)
        {
            const MaxGLBExportMesh& mesh =
                meshes[primitiveIndex];

            if (mesh.nodeGroupIndex !=
                static_cast<int>(
                    groupIndex))
            {
                continue;
            }

            const MaxGLBExportLayout& layout =
                layouts[primitiveIndex];

            if (!firstPrimitiveJson)
            {
                json << ",";
            }

            firstPrimitiveJson = FALSE;

            json
                << "{"
                << "\"attributes\":{"
                << "\"POSITION\":"
                << layout.positionAccessor
                << ",\"NORMAL\":"
                << layout.normalAccessor;

            if (mesh.hasTexcoords)
            {
                json
                    << ",\"TEXCOORD_0\":"
                    << layout.texcoordAccessor;
            }

            json
                << "},"
                << "\"indices\":"
                << layout.indexAccessor;

            if (layout.hasMaterial)
            {
                json
                    << ",\"material\":"
                    << layout.materialIndex;
            }

            json
                << ",\"mode\":4"
                << "}";
        }

        json
            << "]"
            << "}";
    }

    json
        << "],"
        << "\"buffers\":[{"
        << "\"byteLength\":"
        << unpaddedBinaryLength
        << "}],"
        << "\"bufferViews\":[";

    BOOL firstBufferView = TRUE;

    for (size_t meshIndex = 0;
         meshIndex < meshes.size();
         ++meshIndex)
    {
        const MaxGLBExportMesh& mesh =
            meshes[meshIndex];

        const MaxGLBExportLayout& layout =
            layouts[meshIndex];

        if (!firstBufferView)
        {
            json << ",";
        }

        firstBufferView = FALSE;

        json
            << "{"
            << "\"buffer\":0,"
            << "\"byteOffset\":"
            << layout.positionOffset
            << ","
            << "\"byteLength\":"
            << layout.positionLength
            << ","
            << "\"target\":34962"
            << "},"
            << "{"
            << "\"buffer\":0,"
            << "\"byteOffset\":"
            << layout.normalOffset
            << ","
            << "\"byteLength\":"
            << layout.normalLength
            << ","
            << "\"target\":34962"
            << "}";

        if (mesh.hasTexcoords)
        {
            json
                << ",{"
                << "\"buffer\":0,"
                << "\"byteOffset\":"
                << layout.texcoordOffset
                << ","
                << "\"byteLength\":"
                << layout.texcoordLength
                << ","
                << "\"target\":34962"
                << "}";
        }

        json
            << ",{"
            << "\"buffer\":0,"
            << "\"byteOffset\":"
            << layout.indexOffset
            << ","
            << "\"byteLength\":"
            << layout.indexLength
            << ","
            << "\"target\":34963"
            << "}";

        for (int roleIndex = 0;
             roleIndex < MAXGLB_TEXTURE_COUNT;
             ++roleIndex)
        {
            if (!layout.hasTexture[roleIndex])
            {
                continue;
            }

            json
                << ",{"
                << "\"buffer\":0,"
                << "\"byteOffset\":"
                << layout.imageOffset[roleIndex]
                << ","
                << "\"byteLength\":"
                << layout.imageLength[roleIndex]
                << "}";
        }
    }

    json
        << "],"
        << "\"accessors\":[";

    BOOL firstAccessor = TRUE;

    for (size_t meshIndex = 0;
         meshIndex < meshes.size();
         ++meshIndex)
    {
        const MaxGLBExportMesh& mesh =
            meshes[meshIndex];

        const MaxGLBExportLayout& layout =
            layouts[meshIndex];

        if (!firstAccessor)
        {
            json << ",";
        }

        firstAccessor = FALSE;

        json
            << "{"
            << "\"bufferView\":"
            << layout.positionBufferView
            << ","
            << "\"byteOffset\":0,"
            << "\"componentType\":5126,"
            << "\"count\":"
            << (mesh.positions.size() / 3)
            << ","
            << "\"type\":\"VEC3\","
            << "\"min\":["
            << mesh.minimum[0] << ","
            << mesh.minimum[1] << ","
            << mesh.minimum[2] << "],"
            << "\"max\":["
            << mesh.maximum[0] << ","
            << mesh.maximum[1] << ","
            << mesh.maximum[2] << "]"
            << "},"
            << "{"
            << "\"bufferView\":"
            << layout.normalBufferView
            << ","
            << "\"byteOffset\":0,"
            << "\"componentType\":5126,"
            << "\"count\":"
            << (mesh.normals.size() / 3)
            << ","
            << "\"type\":\"VEC3\""
            << "}";

        if (mesh.hasTexcoords)
        {
            json
                << ",{"
                << "\"bufferView\":"
                << layout.texcoordBufferView
                << ","
                << "\"byteOffset\":0,"
                << "\"componentType\":5126,"
                << "\"count\":"
                << (mesh.texcoords.size() / 2)
                << ","
                << "\"type\":\"VEC2\""
                << "}";
        }

        json
            << ",{"
            << "\"bufferView\":"
            << layout.indexBufferView
            << ","
            << "\"byteOffset\":0,"
            << "\"componentType\":5125,"
            << "\"count\":"
            << mesh.indices.size()
            << ","
            << "\"type\":\"SCALAR\""
            << "}";
    }

    json << "]";

    if (nextMaterialIndex > 0)
    {
        json << ",\"materials\":[";

        BOOL firstMaterial = TRUE;

        for (size_t meshIndex = 0;
             meshIndex < meshes.size();
             ++meshIndex)
        {
            const MaxGLBExportMesh& mesh =
                meshes[meshIndex];

            const MaxGLBExportLayout& layout =
                layouts[meshIndex];

            if (!layout.hasMaterial)
            {
                continue;
            }

            if (!firstMaterial)
            {
                json << ",";
            }

            firstMaterial = FALSE;

            json
                << "{"
                << "\"name\":\""
                << EscapeJsonString(
                    mesh.material.name)
                << "\","
                << "\"pbrMetallicRoughness\":{"
                << "\"baseColorFactor\":["
                << mesh.material.baseColor[0] << ","
                << mesh.material.baseColor[1] << ","
                << mesh.material.baseColor[2] << ","
                << mesh.material.baseColor[3]
                << "],"
                << "\"metallicFactor\":"
                << (layout.hasTexture[
                        MAXGLB_TEXTURE_ORM]
                    ? 1
                    : 0)
                << ","
                << "\"roughnessFactor\":1";

            if (layout.hasTexture[
                    MAXGLB_TEXTURE_BASE_COLOR])
            {
                json
                    << ",\"baseColorTexture\":{"
                    << "\"index\":"
                    << layout.textureIndex[
                        MAXGLB_TEXTURE_BASE_COLOR]
                    << "}";
            }

            if (layout.hasTexture[
                    MAXGLB_TEXTURE_ORM])
            {
                json
                    << ",\"metallicRoughnessTexture\":{"
                    << "\"index\":"
                    << layout.textureIndex[
                        MAXGLB_TEXTURE_ORM]
                    << "}";
            }

            json << "}";

            if (layout.hasTexture[
                    MAXGLB_TEXTURE_NORMAL])
            {
                json
                    << ",\"normalTexture\":{"
                    << "\"index\":"
                    << layout.textureIndex[
                        MAXGLB_TEXTURE_NORMAL]
                    << "}";
            }

            if (layout.hasTexture[
                    MAXGLB_TEXTURE_ORM])
            {
                json
                    << ",\"occlusionTexture\":{"
                    << "\"index\":"
                    << layout.textureIndex[
                        MAXGLB_TEXTURE_ORM]
                    << "}";
            }

            if (layout.hasTexture[
                    MAXGLB_TEXTURE_EMISSIVE])
            {
                json
                    << ",\"emissiveTexture\":{"
                    << "\"index\":"
                    << layout.textureIndex[
                        MAXGLB_TEXTURE_EMISSIVE]
                    << "},"
                    << "\"emissiveFactor\":[1,1,1]";
            }

            if (mesh.material.doubleSided)
            {
                json
                    << ",\"doubleSided\":true";
            }

            if (mesh.material.alphaMode ==
                MAXGLB_ALPHA_BLEND)
            {
                json
                    << ",\"alphaMode\":\"BLEND\"";
            }
            else if (mesh.material.alphaMode ==
                MAXGLB_ALPHA_MASK)
            {
                json
                    << ",\"alphaMode\":\"MASK\""
                    << ",\"alphaCutoff\":"
                    << mesh.material.alphaCutoff;
            }

            json << "}";
        }

        json << "]";
    }

    if (nextTextureIndex > 0)
    {
        json
            << ",\"samplers\":[{"
            << "\"magFilter\":9729,"
            << "\"minFilter\":9987,"
            << "\"wrapS\":10497,"
            << "\"wrapT\":10497"
            << "}]";

        json << ",\"textures\":[";

        BOOL firstTexture = TRUE;

        for (size_t meshIndex = 0;
             meshIndex < meshes.size();
             ++meshIndex)
        {
            const MaxGLBExportLayout& layout =
                layouts[meshIndex];

            for (int roleIndex = 0;
                 roleIndex < MAXGLB_TEXTURE_COUNT;
                 ++roleIndex)
            {
                if (!layout.hasTexture[roleIndex])
                {
                    continue;
                }

                if (!firstTexture)
                {
                    json << ",";
                }

                firstTexture = FALSE;

                json
                    << "{"
                    << "\"sampler\":0,"
                    << "\"source\":"
                    << layout.imageIndex[roleIndex]
                    << "}";
            }
        }

        json << "]";

        json << ",\"images\":[";

        BOOL firstImage = TRUE;

        for (size_t meshIndex = 0;
             meshIndex < meshes.size();
             ++meshIndex)
        {
            const MaxGLBExportMesh& mesh =
                meshes[meshIndex];

            const MaxGLBExportLayout& layout =
                layouts[meshIndex];

            for (int roleIndex = 0;
                 roleIndex < MAXGLB_TEXTURE_COUNT;
                 ++roleIndex)
            {
                if (!layout.hasTexture[roleIndex])
                {
                    continue;
                }

                if (!firstImage)
                {
                    json << ",";
                }

                firstImage = FALSE;

                const MaxGLBExportImageData& image =
                    mesh.material.textures[
                        roleIndex];

                json
                    << "{"
                    << "\"name\":\""
                    << EscapeJsonString(
                        image.name)
                    << "\","
                    << "\"bufferView\":"
                    << layout.imageBufferView[
                        roleIndex]
                    << ","
                    << "\"mimeType\":\""
                    << image.mimeType
                    << "\""
                    << "}";
            }
        }

        json << "]";
    }

    json
        << ",\"extras\":{"
        << "\"exportMode\":\""
        << (selectedOnly
                ? "selected"
                : "scene")
        << "\","
        << "\"geometryStage\":9,"
        << "\"materialStage\":\"multiSubObject\""
        << "}"
        << "}";

    const std::string jsonText =
        json.str();

    const size_t jsonPadding =
        (4 - (jsonText.size() % 4)) % 4;

    const size_t paddedJsonLength =
        jsonText.size() +
        jsonPadding;

    const size_t binaryChunkLength =
        binaryData.size();

    const size_t totalLength =
        12 +
        8 +
        paddedJsonLength +
        8 +
        binaryChunkLength;

    if (paddedJsonLength > 0xFFFFFFFFu ||
        binaryChunkLength > 0xFFFFFFFFu ||
        totalLength > 0xFFFFFFFFu)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The generated GLB file would exceed the GLB 2.0 size limit."));
        return FALSE;
    }

    FILE* outputFile = NULL;

    if (_tfopen_s(
            &outputFile,
            outputFilename,
            _T("wb")) != 0 ||
        outputFile == NULL)
    {
        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The GLB output file could not be created."));
        return FALSE;
    }

    const unsigned int glbHeader[3] =
    {
        0x46546C67u,
        2u,
        static_cast<unsigned int>(
            totalLength)
    };

    const unsigned int jsonChunkHeader[2] =
    {
        static_cast<unsigned int>(
            paddedJsonLength),
        0x4E4F534Au
    };

    const unsigned int binaryChunkHeader[2] =
    {
        static_cast<unsigned int>(
            binaryChunkLength),
        0x004E4942u
    };

    BOOL writeSucceeded = TRUE;

    if (fwrite(
            glbHeader,
            sizeof(unsigned int),
            3,
            outputFile) != 3)
    {
        writeSucceeded = FALSE;
    }

    if (writeSucceeded &&
        fwrite(
            jsonChunkHeader,
            sizeof(unsigned int),
            2,
            outputFile) != 2)
    {
        writeSucceeded = FALSE;
    }

    if (writeSucceeded &&
        !WriteCancelableBlock(
            outputFile,
            jsonText.data(),
            jsonText.size(),
            82,
            85,
            _T("Writing GLB metadata..."),
            errorMessage,
            errorMessageCount))
    {
        writeSucceeded = FALSE;
    }

    for (size_t paddingIndex = 0;
         writeSucceeded &&
         paddingIndex < jsonPadding;
         ++paddingIndex)
    {
        const unsigned char paddingByte =
            static_cast<unsigned char>(' ');

        if (fwrite(
                &paddingByte,
                1,
                1,
                outputFile) != 1)
        {
            writeSucceeded = FALSE;
        }
    }

    if (writeSucceeded &&
        fwrite(
            binaryChunkHeader,
            sizeof(unsigned int),
            2,
            outputFile) != 2)
    {
        writeSucceeded = FALSE;
    }

    if (writeSucceeded &&
        !binaryData.empty() &&
        !WriteCancelableBlock(
            outputFile,
            &binaryData[0],
            binaryData.size(),
            86,
            99,
            _T("Writing geometry and embedded textures..."),
            errorMessage,
            errorMessageCount))
    {
        writeSucceeded = FALSE;
    }

    fclose(outputFile);

    if (!writeSucceeded)
    {
        DeleteFile(outputFilename);

        if (!MaxGLBOperationWasCancelled() &&
            (errorMessage == NULL ||
             errorMessage[0] == _T('\0')))
        {
            _tcscpy_s(
                errorMessage,
                errorMessageCount,
                _T("The GLB file could not be written completely."));
        }

        return FALSE;
    }

    return TRUE;
}


class MaxGLBExporter : public SceneExport
{
public:
    int ExtCount()
    {
        return 1;
    }

    const TCHAR* Ext(int /*index*/)
    {
        return _T("GLB");
    }

    const TCHAR* LongDesc()
    {
        return _T("glTF Binary Scene");
    }

    const TCHAR* ShortDesc()
    {
        return _T("GLB Binary Scene");
    }

    const TCHAR* AuthorName()
    {
        return _T("Martin Hoeglund");
    }

    const TCHAR* CopyrightMessage()
    {
        return _T("Copyright (c) 2026 Martin Hoeglund");
    }

    const TCHAR* OtherMessage1()
    {
        return _T("");
    }

    const TCHAR* OtherMessage2()
    {
        return _T("");
    }

    unsigned int Version()
    {
        return 111;
    }

    void ShowAbout(HWND parentWindow)
    {
        MessageBox(
            parentWindow,
            _T("MaxGLB2016\nGLB importer and exporter"),
            _T("About MaxGLB2016"),
            MB_OK | MB_ICONINFORMATION);
    }

    BOOL SupportsOptions(
        int /*extensionIndex*/,
        DWORD options)
    {
        return (options & ~SCENE_EXPORT_SELECTED) == 0;
    }

    int DoExport(
        const TCHAR* outputFilename,
        ExpInterface* /*exportInterface*/,
        Interface* maxInterface,
        BOOL suppressPrompts = FALSE,
        DWORD options = 0)
    {
        const BOOL selectedOnly =
            (options & SCENE_EXPORT_SELECTED) != 0;

        MaxGLBExportSettings exportSettings =
            g_lastExportSettings;

        if (!suppressPrompts)
        {
            if (!ShowMaxGLBExportOptions(
                    maxInterface != NULL
                        ? maxInterface->GetMAXHWnd()
                        : NULL,
                    outputFilename,
                    selectedOnly,
                    &exportSettings))
            {
                return IMPEXP_CANCEL;
            }

            g_lastExportSettings =
                exportSettings;
        }

        TCHAR errorMessage[1024];
        errorMessage[0] = _T('\0');

        g_exportProgressProcessedNodes = 0;
        g_exportNextNodeGroupIndex = 0;

        if (selectedOnly)
        {
            g_exportProgressTotalNodes =
                maxInterface != NULL
                ? maxInterface->GetSelNodeCount()
                : 1;
        }
        else
        {
            g_exportProgressTotalNodes =
                maxInterface != NULL
                ? CountSceneNodesRecursive(
                    maxInterface->GetRootNode())
                : 1;
        }

        if (g_exportProgressTotalNodes <= 0)
        {
            g_exportProgressTotalNodes = 1;
        }

        BeginMaxGLBProgress(
            suppressPrompts
                ? NULL
                : maxInterface,
            _T("MaxGLB2016 Export"));

        UpdateMaxGLBProgress(
            1,
            _T("Collecting scene objects..."));

        TCHAR temporaryFilename[2048];
        temporaryFilename[0] = _T('\0');

        TCHAR actualOutputFilename[2048];
        _tcscpy_s(
            actualOutputFilename,
            _countof(actualOutputFilename),
            outputFilename);

        BOOL usedAlternateOutputFilename =
            FALSE;

        DWORD outputReplacementError =
            ERROR_SUCCESS;

        if (!BuildTemporaryExportFilename(
                outputFilename,
                temporaryFilename,
                _countof(temporaryFilename)))
        {
            EndMaxGLBProgress();

            if (!suppressPrompts)
            {
                MessageBox(
                    maxInterface != NULL
                        ? maxInterface->GetMAXHWnd()
                        : NULL,
                    _T("The temporary export filename could not be created."),
                    _T("MaxGLB2016 Export"),
                    MB_OK | MB_ICONERROR);
            }

            return IMPEXP_FAIL;
        }

        DeleteFile(temporaryFilename);

        std::vector<MaxGLBExportMesh> meshes;

        if (!CollectMeshesForExport(
                maxInterface,
                selectedOnly,
                exportSettings,
                meshes,
                errorMessage,
                _countof(errorMessage)) ||
            !ApplyExportSizeSettings(
                meshes,
                &exportSettings,
                errorMessage,
                _countof(errorMessage)) ||
            !WriteGeometryGlbFile(
                temporaryFilename,
                meshes,
                selectedOnly,
                errorMessage,
                _countof(errorMessage)) ||
            !UpdateMaxGLBProgress(
                99,
                _T("Finalizing output file...")) ||
            !CommitTemporaryExportFile(
                temporaryFilename,
                outputFilename,
                actualOutputFilename,
                _countof(actualOutputFilename),
                &usedAlternateOutputFilename,
                &outputReplacementError,
                errorMessage,
                _countof(errorMessage)))
        {
            const BOOL exportWasCancelled =
                MaxGLBOperationWasCancelled();

            DeleteFile(temporaryFilename);
            EndMaxGLBProgress();

            if (exportWasCancelled)
            {
                return IMPEXP_CANCEL;
            }

            if (!suppressPrompts)
            {
                MessageBox(
                    maxInterface != NULL
                        ? maxInterface->GetMAXHWnd()
                        : NULL,
                    errorMessage,
                    _T("MaxGLB2016 Export"),
                    MB_OK | MB_ICONERROR);
            }

            return IMPEXP_FAIL;
        }

        UpdateMaxGLBProgress(
            100,
            _T("Export complete"));

        EndMaxGLBProgress();

        const size_t exportedObjectCount =
            CountExportNodeGroups(meshes);

        size_t totalVertexCount = 0;
        size_t totalTriangleCount = 0;
        size_t primitivesWithUvs = 0;
        size_t multiMaterialObjects = 0;
        size_t exportedMaterials = 0;
        size_t exportedBaseColorTextures = 0;
        size_t exportedNormalTextures = 0;
        size_t exportedOrmTextures = 0;
        size_t exactOrmRoundTrips = 0;
        size_t exportedEmissiveTextures = 0;
        size_t alphaBlendMaterials = 0;
        size_t alphaMaskMaterials = 0;

        for (size_t meshIndex = 0;
             meshIndex < meshes.size();
             ++meshIndex)
        {
            totalVertexCount +=
                meshes[meshIndex]
                    .positions.size() / 3;

            totalTriangleCount +=
                meshes[meshIndex]
                    .indices.size() / 3;

            if (meshes[meshIndex].hasTexcoords)
            {
                ++primitivesWithUvs;
            }

            if (meshes[meshIndex].material.present)
            {
                ++exportedMaterials;
            }

            if (meshes[meshIndex].material.textures[
                    MAXGLB_TEXTURE_BASE_COLOR].present)
            {
                ++exportedBaseColorTextures;
            }

            if (meshes[meshIndex].material.textures[
                    MAXGLB_TEXTURE_NORMAL].present)
            {
                ++exportedNormalTextures;
            }

            if (meshes[meshIndex].material.textures[
                    MAXGLB_TEXTURE_ORM].present)
            {
                ++exportedOrmTextures;
            }

            if (meshes[meshIndex].material.preservedOriginalOrm)
            {
                ++exactOrmRoundTrips;
            }

            if (meshes[meshIndex].material.textures[
                    MAXGLB_TEXTURE_EMISSIVE].present)
            {
                ++exportedEmissiveTextures;
            }

            if (meshes[meshIndex].material.alphaMode ==
                MAXGLB_ALPHA_BLEND)
            {
                ++alphaBlendMaterials;
            }
            else if (meshes[meshIndex].material.alphaMode ==
                MAXGLB_ALPHA_MASK)
            {
                ++alphaMaskMaterials;
            }
        }

        for (size_t groupIndex = 0;
             groupIndex < exportedObjectCount;
             ++groupIndex)
        {
            if (CountPrimitivesForGroup(
                    meshes,
                    static_cast<int>(
                        groupIndex)) > 1)
            {
                ++multiMaterialObjects;
            }
        }

        if (!suppressPrompts &&
            exportSettings.showSummary)
        {
            TCHAR message[2300];

            _stprintf_s(
                message,
                _countof(message),
                _T("GLB export completed.\n\n")
                _T("Mode: %s\n")
                _T("Max objects / glTF meshes: %u\n")
                _T("glTF primitives: %u\n")
                _T("Multi-material objects: %u\n")
                _T("Export vertices: %u\n")
                _T("Triangles: %u\n")
                _T("Primitives with UV channel 1: %u\n")
                _T("Standard sub-materials: %u\n")
                _T("Base-color textures: %u\n")
                _T("Normal textures: %u\n")
                _T("Packed ORM textures: %u\n")
                _T("Exact imported ORM round-trips: %u\n")
                _T("Emissive textures: %u\n")
                _T("Alpha BLEND materials: %u\n")
                _T("Alpha MASK materials: %u\n\n")
                _T("Export settings:\n")
                _T("- size mode: %s\n")
                _T("- applied uniform scale: %.7g\n")
                _T("- materials: %s\n")
                _T("- textures embedded in GLB: %s\n")
                _T("- alpha mode setting: %s\n")
                _T("- mask cutoff: %.4g\n")
                _T("- animation: not exported\n\n")
                _T("Geometry:\n")
                _T("- evaluated modifier stack\n")
                _T("- object transforms baked into geometry\n")
                _T("- Max Z-up converted to glTF Y-up\n")
                _T("- explicit normals and UV channel 1\n")
                _T("- hard edges and UV seams preserved\n")
                _T("- Face Material IDs exported as glTF primitives\n")
                _T("- Multi/Sub-Object sub-materials preserved\n\n")
                _T("Output:\n%s%s"),
                selectedOnly
                    ? _T("Export Selected")
                    : _T("Export Scene"),
                static_cast<unsigned int>(
                    exportedObjectCount),
                static_cast<unsigned int>(
                    meshes.size()),
                static_cast<unsigned int>(
                    multiMaterialObjects),
                static_cast<unsigned int>(
                    totalVertexCount),
                static_cast<unsigned int>(
                    totalTriangleCount),
                static_cast<unsigned int>(
                    primitivesWithUvs),
                static_cast<unsigned int>(
                    exportedMaterials),
                static_cast<unsigned int>(
                    exportedBaseColorTextures),
                static_cast<unsigned int>(
                    exportedNormalTextures),
                static_cast<unsigned int>(
                    exportedOrmTextures),
                static_cast<unsigned int>(
                    exactOrmRoundTrips),
                static_cast<unsigned int>(
                    exportedEmissiveTextures),
                static_cast<unsigned int>(
                    alphaBlendMaterials),
                static_cast<unsigned int>(
                    alphaMaskMaterials),
                exportSettings.normalizeSize
                    ? _T("normalize largest dimension")
                    : _T("uniform scale factor"),
                exportSettings.appliedUniformScale,
                exportSettings.exportMaterials
                    ? _T("yes")
                    : _T("no"),
                exportSettings.exportTextures
                    ? _T("yes")
                    : _T("no"),
                exportSettings.alphaMode ==
                        MAXGLB_ALPHA_AUTO
                    ? _T("Auto / preserve")
                    : exportSettings.alphaMode ==
                            MAXGLB_ALPHA_OPAQUE
                        ? _T("OPAQUE")
                        : exportSettings.alphaMode ==
                                MAXGLB_ALPHA_MASK
                            ? _T("MASK")
                            : _T("BLEND"),
                exportSettings.alphaCutoff,
                actualOutputFilename,
                usedAlternateOutputFilename
                    ? _T("\n\nNOTE: The requested file was open or locked.\n")
                      _T("The completed export was saved under a new filename instead.")
                    : _T(""));

            MessageBox(
                maxInterface != NULL
                    ? maxInterface->GetMAXHWnd()
                    : NULL,
                message,
                _T("MaxGLB2016 Export"),
                MB_OK | MB_ICONINFORMATION);
        }

        if (!suppressPrompts &&
            !exportSettings.showSummary &&
            usedAlternateOutputFilename)
        {
            TCHAR alternateMessage[2300];

            _stprintf_s(
                alternateMessage,
                _countof(alternateMessage),
                _T("The requested GLB could not be replaced because it is open or locked.\n\n")
                _T("The completed export was saved here instead:\n%s\n\n")
                _T("Close the application using the original file before exporting over it."),
                actualOutputFilename);

            MessageBox(
                maxInterface != NULL
                    ? maxInterface->GetMAXHWnd()
                    : NULL,
                alternateMessage,
                _T("MaxGLB2016 Export"),
                MB_OK | MB_ICONWARNING);
        }

        if (exportSettings.showInExplorer)
        {
            ShowExportedFileInExplorer(
                actualOutputFilename);
        }

        return IMPEXP_SUCCESS;
    }
};


class MaxGLBExporterClassDesc : public ClassDesc
{
public:
    int IsPublic()
    {
        return TRUE;
    }

    void* Create(BOOL /*loading*/ = FALSE)
    {
        return new MaxGLBExporter();
    }

    const TCHAR* ClassName()
    {
        return _T("MaxGLB2016 Exporter");
    }

    SClass_ID SuperClassID()
    {
        return SCENE_EXPORT_CLASS_ID;
    }

    Class_ID ClassID()
    {
        return MAXGLB_EXPORTER_CLASS_ID;
    }

    const TCHAR* Category()
    {
        return _T("Export");
    }

    const TCHAR* InternalName()
    {
        return _T("MaxGLB2016Exporter");
    }

    HINSTANCE HInstance()
    {
        return hInstance;
    }
};


class MaxGLBImporterClassDesc : public ClassDesc
{
public:
    int IsPublic()
    {
        return TRUE;
    }

    void* Create(BOOL /*loading*/ = FALSE)
    {
        return new MaxGLBImporter();
    }

    const TCHAR* ClassName()
    {
        return _T("MaxGLB2016 Importer");
    }

    SClass_ID SuperClassID()
    {
        return SCENE_IMPORT_CLASS_ID;
    }

    Class_ID ClassID()
    {
        return MAXGLB_IMPORTER_CLASS_ID;
    }

    const TCHAR* Category()
    {
        return _T("Import");
    }

    const TCHAR* InternalName()
    {
        return _T("MaxGLB2016Importer");
    }

    HINSTANCE HInstance()
    {
        return hInstance;
    }
};


static MaxGLBImporterClassDesc g_maxGLBImporterClassDesc;
static MaxGLBExporterClassDesc g_maxGLBExporterClassDesc;


BOOL WINAPI DllMain(
    HINSTANCE moduleInstance,
    ULONG reason,
    LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        hInstance = moduleInstance;
        DisableThreadLibraryCalls(moduleInstance);
    }

    return TRUE;
}


extern "C" __declspec(dllexport)
const TCHAR* LibDescription()
{
    return _T("MaxGLB2016 GLB Importer and Exporter");
}


extern "C" __declspec(dllexport)
int LibNumberClasses()
{
    return 2;
}


extern "C" __declspec(dllexport)
ClassDesc* LibClassDesc(int index)
{
    if (index == 0)
    {
        return &g_maxGLBImporterClassDesc;
    }

    if (index == 1)
    {
        return &g_maxGLBExporterClassDesc;
    }

    return NULL;
}


extern "C" __declspec(dllexport)
ULONG LibVersion()
{
    return VERSION_3DSMAX;
}


extern "C" __declspec(dllexport)
ULONG CanAutoDefer()
{
    return TRUE;
}
