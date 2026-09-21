//========================================================================
// PdfAnnotationFlattener.h
// This file is licensed under the GPLv2 or later
//========================================================================
#ifndef PDF_ANNOTATION_FLATTENER_H
#define PDF_ANNOTATION_FLATTENER_H

#include <string>

// Source-level helper using the pinned Poppler Core API; no public library ABI.
namespace PdfAnnotationFlattener {

enum class Error
{
    None,
    InvalidArguments,
    DamagedInput,
    EncryptedInput,
    SignedInput,
    UnsupportedAnnotation,
    InvalidAppearance,
    IoError,
    WriteError,
};

struct Result
{
    Error error = Error::None;
    std::string message;
    int pageNumber = 0; // One-based failing page, or zero for document errors.
    int annotationIndex = 0; // One-based /Annots index, or zero.
    int pageCount = 0;
    int flattenedAnnotations = 0;
    int generatedAppearances = 0;
    int removedPopupAnnotations = 0;
    int preservedAnnotations = 0;
    int preservedHiddenAnnotations = 0;
    bool ok() const { return error == Error::None; }
};

// Flatten the normal appearances in an already-saved, live-state PDF snapshot.
// Input is never written. Output MUST NOT exist (including symlink aliases).
// A sibling temporary file is published only after a successful full rewrite;
// publication requires filesystem hard-link support. Failure leaves no output.
//
// Screen-view policy: Invisible/Hidden/NoView annotations, Link and Widget are
// retained verbatim. Visible supported markup becomes ordinary page content,
// including on print even when its original Print flag was unset. NoRotate is
// baked for the snapshot's native page rotation. NoZoom, ToggleNoView,
// optional-content, interactive/complex annotations and
// signed/encrypted documents are rejected. Ordinary reciprocal Popup containers
// are removed with their flattened parent; replies/independent content fail.
// Missing AP is generated with Core for supported standard markup types only;
// malformed/ambiguous AP is an error. Existing AP is authoritative for
// opacity/blend; /CA is NOT applied again. See README.local-fork.md.
//
// Callers must serialize first, initialize Poppler as usual, and not concurrently
// mutate globalParams or the input file. Paths use the Core PDFDoc convention.
Result flatten(const std::string &inputFileName, const std::string &outputFileName);

}
#endif
