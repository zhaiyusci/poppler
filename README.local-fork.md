# Local Fork Notes

This branch contains downstream changes in addition to upstream Poppler. Keep
those changes generic: consumer application names, UI policy, and product data
must not be introduced into this repository.

## Downstream Extension Areas

- FreeText appearance and font fallback behavior, including Windows and CJK
  handling.
- Annotation behavior exposed through the Qt 6 wrapper.
- `utils/PdfPageSequenceEditor.{h,cc}`, a Core API helper for inserting,
  importing, deleting, moving, and reordering PDF pages.
- `utils/pdfpagesequence.cc`, an optional command-line frontend built when
  `ENABLE_UTILS` is enabled.

The page-sequence editor uses Poppler's unstable Core C++ API and does not
define a stable public ABI. Consumers that compile it directly must use source,
generated private headers, and Poppler libraries from compatible revisions.

## Maintenance Rules

- Use product-neutral file names, namespaces, targets, diagnostics, and tests.
- Keep consumer-side document models, undo/redo, UI, and save orchestration out
  of this repository.
- Rebase or merge upstream changes deliberately and revalidate every use of
  unstable Core APIs.
- Test page insertion, import, deletion, movement, reordering, forms, optional
  content, named objects, output intents, and annotation identity after changes
  to the serializer.
- Commit and publish this repository before updating a consuming repository's
  submodule gitlink.
