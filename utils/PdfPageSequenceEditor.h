//========================================================================
//
// PdfPageSequenceEditor.h
//
// This file is licensed under the GPLv2 or later
//
//========================================================================

#ifndef PDF_PAGE_SEQUENCE_EDITOR_H
#define PDF_PAGE_SEQUENCE_EDITOR_H

#include <memory>
#include <limits>
#include <string>
#include <vector>

class PDFDoc;

namespace PdfPageSequenceEditor {

enum class Error
{
    None,
    InvalidArguments,
    DamagedInput,
    EncryptedInput,
    PageError,
    IoError,
    WriteError,
};

enum class NamedDestinationConflictPolicy
{
    KeepNames,
    AddSuffixes,
};

enum class NamedDestinationView
{
    XYZ,
    FitWidth,
};

struct Result
{
    Error error = Error::None;
    std::string message;
    int inputPageCount = 0;
    int outputPageCount = 0;

    bool ok() const { return error == Error::None; }
};

class LivePageState;
using LivePageStatePtr = std::shared_ptr<LivePageState>;

struct OcrWord
{
    std::string text;
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    // Baseline endpoints in normalized, top-left page coordinates. NaN means
    // that the source did not provide a baseline (for example a manually
    // inserted word).
    double baselineLeft = std::numeric_limits<double>::quiet_NaN();
    double baselineTop = std::numeric_limits<double>::quiet_NaN();
    double baselineRight = std::numeric_limits<double>::quiet_NaN();
    double baselineBottom = std::numeric_limits<double>::quiet_NaN();
};

struct OcrPage
{
    int pageNumber = 0;
    std::vector<OcrWord> words;
};

Result insertBlankPageAfter(const std::string &inputFileName, const std::string &outputFileName, int pageNumber);
Result insertBlankPageAfter(const std::string &inputFileName, const std::string &outputFileName, int pageNumber, double width, double height);
Result insertPdfPageAfter(const std::string &inputFileName, const std::string &outputFileName, int pageNumber, const std::string &insertedFileName, int pageToInsert,
                          NamedDestinationConflictPolicy conflictPolicy = NamedDestinationConflictPolicy::KeepNames);
Result combinePdfFiles(const std::vector<std::string> &inputFileNames, const std::string &outputFileName, NamedDestinationConflictPolicy conflictPolicy = NamedDestinationConflictPolicy::KeepNames);
int pageCount(const std::string &inputFileName, std::string *errorMessage = nullptr);
Result deletePage(const std::string &inputFileName, const std::string &outputFileName, int pageNumber);
Result movePage(const std::string &inputFileName, const std::string &outputFileName, int sourcePageNumber, int destinationPageNumber);
Result reorderPages(const std::string &inputFileName, const std::string &outputFileName, const std::vector<int> &pageOrder);
Result rotatePage(const std::string &inputFileName, const std::string &outputFileName, int pageNumber, int rotationDegrees);
// Page operations on an already-open document. They edit XRef and /Pages in
// memory; LivePageState keeps the small amount of state needed by undo/redo.
Result insertBlankPageAfter(PDFDoc *document, int pageNumber, double width, double height, LivePageStatePtr *state);
Result duplicatePageAfter(PDFDoc *document, int sourcePageNumber, int pageNumber, NamedDestinationConflictPolicy conflictPolicy, LivePageStatePtr *state);
Result insertPdfPageAfter(PDFDoc *document,
                          int pageNumber,
                          const std::string &insertedFileName,
                          int pageToInsert,
                          NamedDestinationConflictPolicy conflictPolicy,
                          LivePageStatePtr *state);
Result detachPage(PDFDoc *document, int pageNumber, LivePageStatePtr *state);
Result detachPage(PDFDoc *document, int pageNumber, const LivePageStatePtr &state);
Result attachPageAfter(PDFDoc *document, int pageNumber, const LivePageStatePtr &state);
Result movePage(PDFDoc *document, int sourcePageNumber, int destinationPageNumber);
Result rotatePage(PDFDoc *document, int pageNumber, int rotationDegrees);
// Changes the size of the page's visible box: the crop box when the page has one
// (its own or inherited) and the media box otherwise. Both boxes are written on
// the page, which overrides anything inherited, and the media box is grown to
// contain the crop box as the format requires. The lower-left corner is kept, so
// existing content stays where it is and the new room appears to the right and
// above. Sizes are default user space units (points) in the page's unrotated
// axes; a smaller box hides what falls outside it without deleting it.
Result setPageSize(PDFDoc *document, int pageNumber, double width, double height);
// Moves each edge of the page's visible box by its own amount, in default user
// space units (points) in the page's unrotated axes: x1Delta and y1Delta move the
// lower-left corner, x2Delta and y2Delta the upper-right one. A caller that anchors
// a corner or an edge passes a zero delta for the edges that must stay, so any
// anchor works. The page's content keeps its own coordinates; only the box that
// decides what is visible changes, and the media box grows to keep containing the
// crop box. A result with no area, or larger than 20000 points per side, is
// rejected. A smaller box only hides what falls outside it.
Result adjustPageBox(PDFDoc *document, int pageNumber, double x1Delta, double y1Delta, double x2Delta, double y2Delta);
// Updates an already-open document without writing it. This lets callers
// change a named destination and another PDF structure atomically.
Result setNamedDestination(PDFDoc *document, const std::string &name, int pageNumber, double normalizedX, double normalizedY, NamedDestinationView view = NamedDestinationView::XYZ);
Result renameNamedDestination(PDFDoc *document, const std::string &oldName, const std::string &newName);
Result deleteNamedDestination(PDFDoc *document, const std::string &name);
// The PDFDoc overloads update an already-open document in memory. They do not
// write or reopen the file; callers can save all accumulated edits together.
Result editInternalLinkDestination(PDFDoc *document, int sourcePageNumber, double linkLeft, double linkTop, double linkRight, double linkBottom, const std::string &destinationName, int destinationPageNumber, double destinationX,
                                   double destinationY);
Result createInternalLink(PDFDoc *document, int sourcePageNumber, double linkLeft, double linkTop, double linkRight, double linkBottom, const std::string &destinationName, int destinationPageNumber, double destinationX, double destinationY);
Result editExternalLinkDestination(PDFDoc *document, int sourcePageNumber, double linkLeft, double linkTop, double linkRight, double linkBottom, const std::string &url);
Result createExternalLink(PDFDoc *document, int sourcePageNumber, double linkLeft, double linkTop, double linkRight, double linkBottom, const std::string &url);
// A cross-file link names its file and a destination inside it: either a page with a
// position on it, or a named destination of that other file. Both belong to the other
// file, so neither is checked against this document; a page is written as the plain
// number the format stores instead of a page reference, and a position is written as it
// is given. When remoteDestinationName is not empty it is written as the name string and
// destinationPageNumber and the position are ignored, because the name already decides
// where in the other file the link lands. "Not empty" therefore means "use the name", as
// that is the only thing an empty page number could not otherwise express.
Result editRemoteLinkDestination(PDFDoc *document, int sourcePageNumber, double linkLeft, double linkTop, double linkRight, double linkBottom, const std::string &fileName, int destinationPageNumber, double destinationX, double destinationY, const std::string &remoteDestinationName = std::string());
Result createRemoteLink(PDFDoc *document, int sourcePageNumber, double linkLeft, double linkTop, double linkRight, double linkBottom, const std::string &fileName, int destinationPageNumber, double destinationX, double destinationY, const std::string &remoteDestinationName = std::string());
Result editLinkRectangle(PDFDoc *document,
                         int sourcePageNumber,
                         double oldLinkLeft,
                         double oldLinkTop,
                         double oldLinkRight,
                         double oldLinkBottom,
                         double newLinkLeft,
                         double newLinkTop,
                         double newLinkRight,
                         double newLinkBottom);
Result deleteLink(PDFDoc *document, int sourcePageNumber, double linkLeft, double linkTop, double linkRight, double linkBottom);
Result addOcrTextLayers(const std::string &inputFileName, const std::string &outputFileName, const std::vector<OcrPage> &pages);
// Reads and replaces the dedicated invisible text streams created by this editor.
// Coordinates are normalized to the rotated crop box. Neither operation writes a file.
Result readOcrTextLayer(PDFDoc *document, int pageNumber, std::vector<OcrWord> *words);
Result replaceOcrTextLayer(PDFDoc *document, int pageNumber, const std::vector<OcrWord> &words);

}

#endif
