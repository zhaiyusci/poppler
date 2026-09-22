//========================================================================
// PdfPageBounds.h -- single-page appearance bounds, GPLv2 or later
//========================================================================
#ifndef PDF_PAGE_BOUNDS_H
#define PDF_PAGE_BOUNDS_H
#include <string>

// Source-level helper for this pinned Poppler Core, not a public library ABI.
namespace PdfPageBounds {
struct Rect
{
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0; // Original PDF user space, in bp.
};
enum class Error
{
    None,
    InvalidArguments,
    DamagedInput,
    UnsupportedInput,
    LimitExceeded,
    IoError,
    WriteError
};
struct Result
{
    bool ok = false;
    Error error = Error::None;
    std::string message;
    Rect logicalBounds, inkBounds, outputBounds;
    bool hasInk = false;
};

// Expand a valid, unencrypted, unsigned, unrotated single-page appearance PDF.
// MediaBox UNION (conservative outline/path/image ink bounds + padding) is
// translated to a zero-origin page without rasterizing or re-typesetting.
// Cubic control hulls, stroke joins, clipping AABBs, and whole image rectangles
// can overestimate ink. Font precision envelopes scale with the effective
// transform; near-singular or >4096:1 text transforms fail explicitly.
// Empty outlines add no ink. This is NOT a general PDF
// editor: annotations/interactivity (Annots/AcroForm) are intentionally removed.
// UserUnit must be 1; geometry must fit the documented bounded inspection area.
// Missing outlines, PostScript XObjects, unsupported filters, unsafe geometry,
// resource/deadline limits, and unsupported document features fail explicitly.
// Input is read-only and must not be concurrently changed. UTF-8 output path
// MUST be new. A sibling temporary PDF is verified then atomically published
// with a no-replace hard link (filesystem hard-link support required).
Result expandAppearance(const std::string &utf8Input, const std::string &utf8Output, double padding = 1.0);
}
#endif
