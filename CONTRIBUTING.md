# Contributing to MaxGLB2016

Thanks for helping improve MaxGLB2016.

## Project scope

Please keep contributions aligned with the project's deliberately narrow goal:

> A dependable static-model GLB workflow for Autodesk 3ds Max 2016.

Geometry, transforms, hierarchy, UVs, normals, material IDs, Standard materials, opacity, textures, and reliable import/export round trips are the main priorities.

Large additions such as animation, skinning, morph targets, cameras, lights, or a complete Physical Material system should be discussed before implementation.

## Before opening a pull request

1. Open an issue for substantial changes.
2. Explain the user problem and the intended behavior.
3. Keep changes focused and avoid unrelated cleanup.
4. Build `Release | x64` with the Autodesk 3ds Max 2016 SDK.
5. Test import and export in a fresh Max scene.
6. Validate exported GLBs in at least one independent viewer.
7. Do not commit Autodesk SDK files, local test assets, build output, personal paths, or generated Visual Studio databases.

## Compatibility

The code must remain compatible with the Visual C++ toolchain used by the 3ds Max 2016 SDK. Avoid introducing modern language or library requirements that Visual Studio 2012 cannot compile.

## Useful regression tests

When relevant, test with official Khronos assets such as:

- `TextureTransformMultiTest.glb`
- `TextureSettingsTest.glb`
- `ChronographWatch.glb`

Do not add third-party sample assets to the repository unless their license clearly permits redistribution and the required attribution is included.

## Code and repository hygiene

- Use clear C++ names and comments for non-obvious Max/glTF conversions.
- Preserve existing error handling and cancellation behavior.
- Keep public commits free of private paths and credentials.
- Do not commit `bin/`, `obj/`, PDB, SDF, SUO, or local backup files.
- Update `CHANGELOG.md` for user-visible changes.
- Update `THIRD_PARTY_NOTICES.md` when adding a dependency.

## Bug reports

A useful bug report includes:

- 3ds Max version and Windows version
- Import or export steps
- Expected and actual result
- A screenshot when visual behavior is involved
- A small redistributable test file, or a link to a public test asset
- Whether the problem occurs in a second glTF viewer
