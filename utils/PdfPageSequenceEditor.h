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

}

#endif
