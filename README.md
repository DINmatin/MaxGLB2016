# MaxGLB2016

GLB import and export for Autodesk 3ds Max 2016, focused on practical static-model workflows.

<p align="center">
  <img src="docs/images/hero-preview.jpg"
       alt="MaxGLB2016 importing and exporting a GLB model in Autodesk 3ds Max 2016"
       width="100%">
</p>

> **Status:** early public preview. MaxGLB2016 is intentionally narrow in scope: static triangle-mesh models, useful material conversion, and reliable GLB round-trips in 3ds Max 2016.

## Why this project exists

Modern GLB assets are still useful in older production pipelines, but Autodesk 3ds Max 2016 has no built-in glTF/GLB workflow.

MaxGLB2016 adds a native x64 importer and exporter without trying to turn Max 2016 into a complete modern glTF authoring environment.

The goal is simple:

- import one or more static GLB models,
- optimize or edit them in 3ds Max,
- arrange the scene,
- export the complete scene or selected objects back to GLB.

## Current features

### Import

- Binary glTF 2.0 files (`.glb`)
- Embedded geometry and embedded PNG/JPEG textures
- Multiple scene nodes
- Multiple mesh primitives
- Optional parent node
- Optional hierarchy preservation
- Standard glTF Y-up to 3ds Max Z-up conversion
- Additional import orientation controls
- Optional model-size normalization
- `POSITION`
- `NORMAL`
- `TEXCOORD_0`
- `TEXCOORD_1`
- Explicit 3ds Max normals
- Multiple primitives converted to Material IDs
- Automatic Multi/Sub-Object materials
- Base-color textures
- Normal textures
- Metallic/roughness textures
- Occlusion textures
- Emissive textures
- Alpha modes and alpha cutoff
- Base-color alpha handling
- `doubleSided`
- `KHR_texture_transform`
- Sampler wrap modes:
  - repeat
  - clamp-to-edge
  - mirrored repeat
- Sampler filtering preserved for round-trip export
- Closest available 3ds Max Bitmap fallback for sampler filtering
- `KHR_materials_transmission` factor represented with a Standard-material glass fallback
- Metallic and roughness factors preserved for round trips
- Progress display
- Cancellation
- Single-step undo for imports

### Export

- Complete scene or selected nodes
- Embedded GLB geometry
- Embedded textures
- Optional transform baking
- Optional transform and hierarchy preservation
- Material IDs and Multi/Sub-Object materials
- Base-color textures
- Normal textures
- Metallic/roughness textures
- Occlusion textures
- Emissive textures
- Opacity and alpha settings
- `TEXCOORD_0`
- `TEXCOORD_1`
- `KHR_texture_transform` round trips
- Sampler wrap/filter round trips
- `doubleSided`
- `KHR_materials_transmission` factor round trips
- Metallic and roughness factor round trips
- Progress display
- Cancellation
- Optional export summary
- Optional reveal-in-Explorer after export
- Temporary-file export before replacing the final destination file

## Deliberate limitations

MaxGLB2016 is not intended to compete with Blender or newer DCC glTF pipelines.

The current public-preview scope does **not** include:

- text-based `.gltf` scenes with external resources,
- animation,
- skinning or skeletal rigs,
- morph targets,
- cameras,
- lights,
- full Physical Material conversion,
- complete support for every glTF extension,
- authoring of `KHR_materials_clearcoat`,
- authoring of `KHR_materials_volume`,
- authoring of `KHR_materials_ior`,
- transmission textures.

Unsupported material features may use a visual 3ds Max fallback or remain unavailable for editing.

Always keep the original GLB and test important production assets before replacing them.

## Installation

### Prebuilt release

1. Download the release ZIP for Autodesk 3ds Max 2016 x64.
2. Extract `MaxGLB2016.dle`.
3. Copy it into a 3ds Max plug-in directory, or add its folder under:

   `Customize → Configure System Paths → 3rd Party Plug-Ins`

4. Restart 3ds Max 2016.
5. Use **File → Import** or **File → Export** and select GLB.

PDB files are not required unless you are debugging the plug-in.

## Building from source

### Requirements

- Autodesk 3ds Max 2016 SDK
- Visual Studio 2012 / Visual C++ toolchain compatible with the Max 2016 SDK
- Windows x64
- Git

`cgltf` is included under `third_party/cgltf`. Git submodules are not required.

### SDK environment variable

Create a user or system environment variable named:

```text
MAXSDK_2016
```

Set it to the root directory of the Autodesk 3ds Max 2016 SDK.

That directory must contain the SDK `ProjectSettings` directory used by the Autodesk property sheets.

Example shape only:

```text
D:\SDKs\3ds Max 2016 SDK
```

Do not commit personal SDK paths into the project files.

### Build steps

1. Open `MaxGLB2016.sln`.
2. Select `Release | x64`.
3. Build the solution.
4. The plug-in is created at:

```text
src\MaxGLB.Importer\bin\x64\Release\MaxGLB2016.dle
```

## Testing

The project has been tested with official Khronos sample assets, including:

- `TextureTransformMultiTest.glb`
- `TextureSettingsTest.glb`
- `ChronographWatch.glb`

The sample assets themselves are not included in this repository.

For important files, validate exports in more than one independent glTF viewer.

## Repository layout

```text
MaxGLB2016.sln
src/MaxGLB.Importer/    Plug-in source and Visual Studio project
third_party/cgltf/      Bundled cgltf source and license
docs/                   Documentation and screenshots
```

## Contributing

Bug reports, focused fixes, documentation improvements, and reproducible test cases are welcome.

Please read [CONTRIBUTING.md](CONTRIBUTING.md) before submitting a pull request.

The project intentionally prioritizes static-model reliability over a broad feature list.

## License

MaxGLB2016 is released under the MIT License.

See [LICENSE](LICENSE).

Third-party notices are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Author

Martin Hoeglund
