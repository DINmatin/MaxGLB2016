#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>
#include <limits.h>
#include <math.h>
#include <map>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <locale>

#include "../../third_party/cgltf/cgltf.h"

#include <max.h>
#include <impexp.h>
#include <MeshNormalSpec.h>
#include <stdmat.h>
#include <iparamb2.h>
#include <istdplug.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

HINSTANCE hInstance = NULL;

// Import is single-threaded in 3ds Max 2016. This index keeps extracted
// texture filenames unique when one GLB contains several mesh objects.
static int g_importTextureSetIndex = 0;

// Eigene, dauerhafte Class-ID fuer MaxGLB2016.
// Diese Werte spaeter nicht mehr veraendern.
#define MAXGLB_IMPORTER_CLASS_ID \
    Class_ID(0x7a4e21c3, 0x4f1b6d92)

// Separate permanent Class-ID for the SceneExport plug-in.
// Never change this after the public release.
#define MAXGLB_EXPORTER_CLASS_ID \
    Class_ID(0x2b6f4d81, 0x71a35ce4)

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


static Point3 RotateMaxSceneXMinus90(
    const Point3& point)
{
    // Rotate the complete imported scene around the world origin
    // by -90 degrees on the Max X axis.
    //
    // x' = x
    // y' = z
    // z' = -y
    return Point3(
        point.x,
        point.z,
        -point.y);
}


static Point3 ConvertGltfLocalPositionToMax(
    const cgltf_float* position)
{
    // First convert glTF Y-up coordinates to Max Z-up coordinates.
    Point3 maxPosition(
        static_cast<float>(position[0]),
        static_cast<float>(-position[2]),
        static_cast<float>(position[1]));

    // The tested Max 2016 scene requires one additional global
    // -90 degree X rotation around 0,0,0.
    return RotateMaxSceneXMinus90(maxPosition);
}


static Point3 ConvertGltfLocalNormalToMax(
    const cgltf_float* normal)
{
    Point3 maxNormal(
        static_cast<float>(normal[0]),
        static_cast<float>(-normal[2]),
        static_cast<float>(normal[1]));

    // Apply the same global scene rotation to explicit normals.
    maxNormal = RotateMaxSceneXMinus90(maxNormal);

    const float lengthSquared =
        maxNormal.x * maxNormal.x +
        maxNormal.y * maxNormal.y +
        maxNormal.z * maxNormal.z;

    if (lengthSquared > 1.0e-20f)
    {
        const float inverseLength =
            1.0f / sqrtf(lengthSquared);

        maxNormal *= inverseLength;
    }

    return maxNormal;
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



static BOOL BuildTexturePath(
    const TCHAR* sourceFilename,
    const TCHAR* roleName,
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
            g_importTextureSetIndex,
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

    if (!BuildTexturePath(
            sourceFilename,
            roleName,
            extension,
            outputPath,
            outputPathCount,
            errorMessage,
            errorMessageCount))
    {
        return FALSE;
    }

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

    if (!BuildTexturePath(
            sourceFilename,
            _T("Occlusion"),
            _T(".png"),
            outOcclusionPath,
            outOcclusionPathCount,
            errorMessage,
            errorMessageCount) ||
        !BuildTexturePath(
            sourceFilename,
            _T("Glossiness"),
            _T(".png"),
            outGlossinessPath,
            outGlossinessPathCount,
            errorMessage,
            errorMessageCount) ||
        !BuildTexturePath(
            sourceFilename,
            _T("Metallic"),
            _T(".png"),
            outMetallicPath,
            outMetallicPathCount,
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

    maxMaterial->SetOpacity(
        opacity,
        timeValue);

    BOOL hasBaseColorTexture = FALSE;
    BOOL hasNormalTexture = FALSE;
    BOOL hasOrmChannels = FALSE;
    BOOL hasEmissiveTexture = FALSE;

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

        for (cgltf_size primitiveIndex = 0;
             primitiveIndex < sourceMesh->primitives_count;
             ++primitiveIndex)
        {
            cgltf_primitive* primitive =
                &sourceMesh->primitives[
                    primitiveIndex];

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
                positions->type != cgltf_type_vec3 ||
                positions->count == 0)
            {
                continue;
            }

            if (primitiveIndex >
                    static_cast<cgltf_size>(INT_MAX) ||
                importStats->nextObjectIndex ==
                    INT_MAX)
            {
                _tcscpy_s(
                    errorMessage,
                    errorMessageCount,
                    _T("The GLB contains too many mesh primitives."));
                return FALSE;
            }

            const int objectIndex =
                importStats->nextObjectIndex++;

            if (!ImportPrimitiveAsNode(
                    data,
                    sourceFilename,
                    sourceNode,
                    sourceMesh,
                    primitive,
                    static_cast<int>(
                        primitiveIndex),
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
         childIndex < sourceNode->children_count;
         ++childIndex)
    {
        if (!ImportNodeMeshesRecursive(
                data,
                sourceFilename,
                sourceNode->children[childIndex],
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
        return 100;
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

        cgltf_result parseResult =
            cgltf_parse(
                &options,
                fileBytes,
                static_cast<cgltf_size>(fileSize),
                &data);

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

        MaxGLBImportStats importStats;

        TCHAR importError[512];
        importError[0] = _T('\0');

        BOOL importSucceeded =
            ImportAllSceneMeshes(
                data,
                filename,
                maxInterface,
                &importStats,
                importError,
                _countof(importError));

        cgltf_free(data);
        free(fileBytes);

        if (!importSucceeded)
        {
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
                _T("- every triangle primitive becomes a Max object\n")
                _T("- original node names are preserved\n")
                _T("- full parent hierarchy is baked into geometry\n")
                _T("- global scene rotation X = -90 degrees\n")
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
                importStats.emissiveTextures);

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
    BOOL alphaBlend;
    BOOL preservedOriginalOrm;

    float baseColor[4];

    std::string name;

    MaxGLBExportImageData textures[
        MAXGLB_TEXTURE_COUNT];

    MaxGLBExportMaterial()
        : present(FALSE)
        , doubleSided(FALSE)
        , alphaBlend(FALSE)
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
        : hasTexcoords(FALSE)
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
    INode* node,
    TimeValue timeValue,
    MaxGLBExportMaterial* outputMaterial,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (node == NULL ||
        outputMaterial == NULL)
    {
        return TRUE;
    }

    Mtl* nodeMaterial =
        node->GetMtl();

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
            node->GetName());

    if (outputMaterial->name.empty())
    {
        outputMaterial->name =
            "MaxGLB_Material";
    }
    else
    {
        outputMaterial->name +=
            "_Material";
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

    if (outputMaterial->baseColor[3] <
        0.999f)
    {
        outputMaterial->alphaBlend =
            TRUE;
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

    if (opacityBitmap != NULL)
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

            combinedBaseColor.rgba[
                alphaIndex] =
                    static_cast<unsigned char>(
                        (existingAlpha *
                         opacity) / 255);
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

        outputMaterial->alphaBlend =
            TRUE;
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
                    outputMaterial->alphaBlend =
                        TRUE;
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


static BOOL TryBuildExportMesh(
    INode* node,
    TimeValue timeValue,
    MaxGLBExportMesh* outputMesh,
    BOOL* outWasGeometry,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
    if (outWasGeometry != NULL)
    {
        *outWasGeometry = FALSE;
    }

    if (node == NULL || outputMesh == NULL)
    {
        return TRUE;
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

    outputMesh->name =
        ConvertTcharToUtf8(
            node->GetName());

    if (outputMesh->name.empty())
    {
        outputMesh->name =
            "MaxGLB_Mesh";
    }

    if (!CaptureMaterialMaps(
            node,
            timeValue,
            &outputMesh->material,
            errorMessage,
            errorMessageCount))
    {
        if (deleteConvertedObject)
        {
            triangleObject->DeleteThis();
        }

        return FALSE;
    }

    const Matrix3 objectTransform =
        node->GetObjectTM(timeValue);

    const BOOL reverseWinding =
        GetMatrixDeterminant3x3(
            objectTransform) < 0.0f;

    // Channel 1 is the traditional Max diffuse UV channel.
    outputMesh->hasTexcoords =
        mesh.getNumTVerts() > 0 &&
        mesh.tVerts != NULL &&
        mesh.tvFace != NULL;

    const int textureVertexCount =
        mesh.getNumTVerts();

    // This preserves existing explicit normals and also builds normals
    // from smoothing groups when a mesh has no explicit normal data.
    mesh.SpecifyNormals();

    MeshNormalSpec* normalSpec =
        mesh.GetSpecifiedNormals();

    if (normalSpec != NULL)
    {
        normalSpec->SetParent(&mesh);
        normalSpec->CheckNormals();
    }

    const size_t outputVertexCount =
        static_cast<size_t>(faceCount) * 3;

    outputMesh->positions.reserve(
        outputVertexCount * 3);

    outputMesh->normals.reserve(
        outputVertexCount * 3);

    if (outputMesh->hasTexcoords)
    {
        outputMesh->texcoords.reserve(
            outputVertexCount * 2);
    }

    outputMesh->indices.reserve(
        outputVertexCount);

    BOOL boundsInitialized = FALSE;

    // Reuse a glTF vertex whenever all attributes are identical.
    // This welds vertices inside continuous UV/smoothing regions while
    // preserving UV seams and hard normal boundaries.
    std::map<MaxGLBExportVertexKey, unsigned int>
        exportedVertexLookup;

    for (int faceIndex = 0;
         faceIndex < faceCount;
         ++faceIndex)
    {
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
                    mesh.getVert(sourceVertexIndex) *
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
                sqrtf(fallbackLengthSquared);
        }
        else
        {
            fallbackFaceNormal =
                Point3(0.0f, 1.0f, 0.0f);
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
                fallbackPositions[sourceCorner];

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

            if (outputMesh->hasTexcoords)
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

                // Reverse the importer's V conversion.
                gltfV =
                    1.0f - maxUv.y;
            }

            const MaxGLBExportVertexKey vertexKey(
                gltfPosition,
                gltfNormal,
                outputMesh->hasTexcoords,
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
                        outputMesh->positions.size() / 3);

                outputMesh->positions.push_back(
                    gltfPosition.x);

                outputMesh->positions.push_back(
                    gltfPosition.y);

                outputMesh->positions.push_back(
                    gltfPosition.z);

                outputMesh->normals.push_back(
                    gltfNormal.x);

                outputMesh->normals.push_back(
                    gltfNormal.y);

                outputMesh->normals.push_back(
                    gltfNormal.z);

                if (outputMesh->hasTexcoords)
                {
                    outputMesh->texcoords.push_back(
                        gltfU);

                    outputMesh->texcoords.push_back(
                        gltfV);
                }

                exportedVertexLookup.insert(
                    std::make_pair(
                        vertexKey,
                        exportVertexIndex));

                if (!boundsInitialized)
                {
                    outputMesh->minimum[0] =
                        outputMesh->maximum[0] =
                            gltfPosition.x;

                    outputMesh->minimum[1] =
                        outputMesh->maximum[1] =
                            gltfPosition.y;

                    outputMesh->minimum[2] =
                        outputMesh->maximum[2] =
                            gltfPosition.z;

                    boundsInitialized = TRUE;
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
                            outputMesh->minimum[axisIndex])
                        {
                            outputMesh->minimum[axisIndex] =
                                positionValues[axisIndex];
                        }

                        if (positionValues[axisIndex] >
                            outputMesh->maximum[axisIndex])
                        {
                            outputMesh->maximum[axisIndex] =
                                positionValues[axisIndex];
                        }
                    }
                }
            }

            outputMesh->indices.push_back(
                exportVertexIndex);
        }
    }

    if (deleteConvertedObject)
    {
        triangleObject->DeleteThis();
    }

    if (outWasGeometry != NULL)
    {
        *outWasGeometry = TRUE;
    }

    return TRUE;
}


static BOOL CollectSceneMeshesRecursive(
    INode* parentNode,
    TimeValue timeValue,
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

        MaxGLBExportMesh exportMesh;
        BOOL wasGeometry = FALSE;

        if (!TryBuildExportMesh(
                childNode,
                timeValue,
                &exportMesh,
                &wasGeometry,
                errorMessage,
                errorMessageCount))
        {
            return FALSE;
        }

        if (wasGeometry)
        {
            outputMeshes.push_back(
                exportMesh);
        }

        if (!CollectSceneMeshesRecursive(
                childNode,
                timeValue,
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

            MaxGLBExportMesh exportMesh;
            BOOL wasGeometry = FALSE;

            if (!TryBuildExportMesh(
                    selectedNode,
                    timeValue,
                    &exportMesh,
                    &wasGeometry,
                    errorMessage,
                    errorMessageCount))
            {
                return FALSE;
            }

            if (wasGeometry)
            {
                outputMeshes.push_back(
                    exportMesh);
            }
        }
    }
    else
    {
        if (!CollectSceneMeshesRecursive(
                maxInterface->GetRootNode(),
                timeValue,
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


static BOOL WriteGeometryGlbFile(
    const TCHAR* outputFilename,
    const std::vector<MaxGLBExportMesh>& meshes,
    BOOL selectedOnly,
    TCHAR* errorMessage,
    size_t errorMessageCount)
{
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
         nodeIndex < meshes.size();
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
         nodeIndex < meshes.size();
         ++nodeIndex)
    {
        if (nodeIndex > 0)
        {
            json << ",";
        }

        json
            << "{"
            << "\"mesh\":"
            << nodeIndex
            << ",\"name\":\""
            << EscapeJsonString(
                meshes[nodeIndex].name)
            << "\""
            << "}";
    }

    json
        << "],"
        << "\"meshes\":[";

    for (size_t meshIndex = 0;
         meshIndex < meshes.size();
         ++meshIndex)
    {
        if (meshIndex > 0)
        {
            json << ",";
        }

        const MaxGLBExportMesh& mesh =
            meshes[meshIndex];

        const MaxGLBExportLayout& layout =
            layouts[meshIndex];

        json
            << "{"
            << "\"name\":\""
            << EscapeJsonString(mesh.name)
            << "\","
            << "\"primitives\":[{"
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
            << "}]"
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

            if (mesh.material.alphaBlend)
            {
                json
                    << ",\"alphaMode\":\"BLEND\"";
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
        << "\"geometryStage\":5,"
        << "\"materialStage\":\"allMaps\""
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
        fwrite(
            jsonText.data(),
            1,
            jsonText.size(),
            outputFile) !=
                jsonText.size())
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
        fwrite(
            &binaryData[0],
            1,
            binaryData.size(),
            outputFile) !=
                binaryData.size())
    {
        writeSucceeded = FALSE;
    }

    fclose(outputFile);

    if (!writeSucceeded)
    {
        DeleteFile(outputFilename);

        _tcscpy_s(
            errorMessage,
            errorMessageCount,
            _T("The GLB file could not be written completely."));
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
        return 104;
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

        TCHAR errorMessage[1024];
        errorMessage[0] = _T('\0');

        std::vector<MaxGLBExportMesh> meshes;

        if (!CollectMeshesForExport(
                maxInterface,
                selectedOnly,
                meshes,
                errorMessage,
                _countof(errorMessage)) ||
            !WriteGeometryGlbFile(
                outputFilename,
                meshes,
                selectedOnly,
                errorMessage,
                _countof(errorMessage)))
        {
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

        size_t totalVertexCount = 0;
        size_t totalTriangleCount = 0;
        size_t meshesWithUvs = 0;
        size_t exportedMaterials = 0;
        size_t exportedBaseColorTextures = 0;
        size_t exportedNormalTextures = 0;
        size_t exportedOrmTextures = 0;
        size_t exactOrmRoundTrips = 0;
        size_t exportedEmissiveTextures = 0;
        size_t alphaMaterials = 0;

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
                ++meshesWithUvs;
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

            if (meshes[meshIndex].material.alphaBlend)
            {
                ++alphaMaterials;
            }
        }

        if (!suppressPrompts)
        {
            TCHAR message[1200];

            _stprintf_s(
                message,
                _countof(message),
                _T("GLB geometry export completed.\n\n")
                _T("Mode: %s\n")
                _T("Meshes: %u\n")
                _T("Export vertices: %u\n")
                _T("Triangles: %u\n")
                _T("Meshes with UV channel 1: %u\n")
                _T("Standard materials: %u\n")
                _T("Base-color textures: %u\n")
                _T("Normal textures: %u\n")
                _T("Packed ORM textures: %u\n")
                _T("Exact imported ORM round-trips: %u\n")
                _T("Emissive textures: %u\n")
                _T("Alpha/opacity materials: %u\n\n")
                _T("Included:\n")
                _T("- evaluated modifier stack\n")
                _T("- object transforms baked into geometry\n")
                _T("- Max Z-up converted to glTF Y-up\n")
                _T("- indexed triangle geometry\n")
                _T("- explicit and smoothing-group normals\n")
                _T("- UV channel 1 as TEXCOORD_0\n")
                _T("- identical position/normal/UV tuples reused\n")
                _T("- hard edges and UV seams preserved\n")
                _T("- Standard diffuse color as baseColorFactor\n")
                _T("- diffuse/opacity merged into baseColor RGBA\n")
                _T("- normal bitmap exported separately\n")
                _T("- original imported ORM reused byte-for-byte when available\n")
                _T("- otherwise ambient/glossiness/specular-level packed as ORM\n")
                _T("- glossiness inverted to glTF roughness\n")
                _T("- self-illumination bitmap exported as emissive\n")
                _T("- opacity factor and alphaMode BLEND preserved\n\n")
                _T("Current limits:\n")
                _T("- direct BitmapTex maps only\n")
                _T("- separate opacity map must match diffuse dimensions\n")
                _T("- ORM source maps must share dimensions\n\n")
                _T("Output:\n%s"),
                selectedOnly
                    ? _T("Export Selected")
                    : _T("Export Scene"),
                static_cast<unsigned int>(
                    meshes.size()),
                static_cast<unsigned int>(
                    totalVertexCount),
                static_cast<unsigned int>(
                    totalTriangleCount),
                static_cast<unsigned int>(
                    meshesWithUvs),
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
                    alphaMaterials),
                outputFilename);

            MessageBox(
                maxInterface != NULL
                    ? maxInterface->GetMAXHWnd()
                    : NULL,
                message,
                _T("MaxGLB2016 Export"),
                MB_OK | MB_ICONINFORMATION);
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
