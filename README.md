# c-corvus-json-schema

A [Bowtie](https://github.com/bowtie-json-schema/bowtie) test harness for the corvus-json-schema C library, part of
[Corvus.JsonSchema](https://github.com/corvus-dotnet/Corvus.JsonSchema): a C ABI over the corvus-json-schema Rust
crate.

Its image is published to `ghcr.io/bowtie-json-schema/c-corvus-json-schema` and run via
`bowtie run -i c-corvus-json-schema`.

The harness compiles each case's schema with the case's `registry` as the document resolver and validates each
instance. For `annotations` output it evaluates through a verbose collector and reports each annotation with its
instance location and `#…` keyword location. Requests are read with a small JSON scanner that keeps each value's text
as Bowtie wrote it, so schemas and instances reach the library unchanged.

The image links the library statically from its latest release (the musl package for the platform, tagged
`capi-v<version>`); the `IMPLEMENTATION_VERSION` build argument pins another.
