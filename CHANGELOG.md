# Changelog

All notable changes to MaxGLB2016 will be documented in this file.

The project follows semantic versioning where practical during public releases.

## [Unreleased]

### Planned

- First public GitHub release package
- Hero preview and installation screenshots
- Additional validation and regression testing

## [0.1.0] - Unreleased

Initial public-preview feature set.

### Added

- Native GLB importer and exporter for Autodesk 3ds Max 2016 x64
- Static triangle-mesh import and export
- Multiple scene nodes and mesh primitives
- Optional hierarchy preservation and transform baking
- Import orientation controls and model-size normalization
- `POSITION`, explicit `NORMAL`, `TEXCOORD_0`, and `TEXCOORD_1`
- Multi/Sub-Object material generation from multiple glTF primitives
- Base-color, normal, metallic/roughness, occlusion, emissive, and opacity workflows
- Embedded PNG and JPEG texture extraction/import
- Embedded texture export
- Alpha modes and alpha-cutoff handling
- `doubleSided` import/export
- `KHR_texture_transform` import/export round trips
- Repeat, clamp-to-edge, and mirrored-repeat sampler support
- Sampler filter preservation
- `KHR_materials_transmission` factor fallback and round trips
- Metallic and roughness factor preservation
- Progress display and cancellation
- Import undo transaction
- Export summaries and optional reveal-in-Explorer
- Safe temporary-file export before replacing the destination GLB

### Known limitations

- Static-model workflow only
- Binary `.glb` only
- No animation, skinning, morph targets, cameras, or lights
- No full Physical Material workflow
- No complete support for all glTF material extensions
- No transmission texture support
