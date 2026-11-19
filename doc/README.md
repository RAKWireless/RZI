# Documentation

Status: index

<a id="rzi-docs-home"></a>

Start with the [repository setup guide](https://github.com/RAKWireless/RZI#setup),
then choose a topic below. Documentation is maintained in English.

| Section | Contents |
|---|---|
| [Architecture and boards](architecture/index.md) | SDK ownership, boards, device trees, boot, power, USP, patches. |
| [API design](api-design/index.md) | Design principles, ownership, call paths, and backend behavior. |
| [Service APIs](api/index.md) | Contracts, backends, storage, power, and AT. |
| [Development and maintenance](development/index.md) | Coding, Doxygen, CI, reviews, releases, SBOM, security. |
| [Compatibility and migration](compatibility/index.md) | AT compatibility, RUI3 migration mappings, and backend differences. |

Each page identifies its status: normative rules, implemented behavior,
evaluation findings, an index, or superseded material. Public declarations
under include/rzi remain the ABI. Source review, build results, host tests,
and hardware acceptance are separate forms of evidence.

## Build the documentation

From the repository root:

```sh
tools/docs/generate-doxygen.sh
tools/docs/generate-mdbook.sh
```

Doxygen writes doc/doxygen/html/index.html. mdBook writes doc/book/index.html.
Both outputs are generated and ignored by Git. Neither build requires SVG
assets or a diagram-rendering plugin. See [documentation rules](development/api-docs.md).
