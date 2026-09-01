//========================================================================
//
// PdfPageSequenceEditor.cc
//
// This file is licensed under the GPLv2 or later
//
// A small page-sequence editing library. This intentionally lives next to
// pdfunite instead of changing Poppler's core ABI.
//
//========================================================================

#include "PdfPageSequenceEditor.h"

#include "config.h"
#include <poppler-config.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <GlobalParams.h>
#include <PDFDoc.h>

#include "Array.h"
#include "Dict.h"
#include "Error.h"
#include "Object.h"
#include "Page.h"

namespace
{

struct PageEntry
{
    Object page;
    unsigned int numOffset = 0;
};

PdfPageSequenceEditor::Result makeError(PdfPageSequenceEditor::Error error, std::string message, int inputPageCount = 0)
{
    return PdfPageSequenceEditor::Result { error, std::move(message), inputPageCount, 0 };
}

void ensureGlobalParams()
{
    if (!globalParams) {
        globalParams = std::make_unique<GlobalParams>();
    }
}

Object makeBoxArray(XRef *xref, const PDFRectangle *box)
{
    auto *array = new Array(xref);
    array->add(Object(box->x1));
    array->add(Object(box->y1));
    array->add(Object(box->x2));
    array->add(Object(box->y2));
    return Object(array);
}

Object makeBlankPage(XRef *xref, const PDFRectangle *mediaBox, const PDFRectangle *cropBox, int rotate)
{
    auto *dict = new Dict(xref);
    dict->add("Type", Object(objName, "Page"));
    // Placeholder rewritten to the generated /Pages root when pages are emitted.
    dict->add("Parent", Object(Ref { .num = 0, .gen = 0 }));
    dict->add("MediaBox", makeBoxArray(xref, mediaBox));
    if (cropBox != nullptr) {
        dict->add("CropBox", makeBoxArray(xref, cropBox));
        dict->add("TrimBox", makeBoxArray(xref, cropBox));
    } else {
        dict->add("TrimBox", makeBoxArray(xref, mediaBox));
    }
    dict->add("Rotate", Object(rotate));
    dict->add("Resources", Object(new Dict(xref)));
    return Object(dict);
}

bool markCatalogObjects(PDFDoc *doc, XRef *yRef, XRef *countRef, Object *intents, Object *acroForm, Object *ocProperties, Object *names)
{
    Object catObj = doc->getXRef()->getCatalog();
    if (!catObj.isDict()) {
        error(errSyntaxError, -1, "XRef's Catalog is not a dictionary.");
        return false;
    }

    Dict *catDict = catObj.getDict();
    *intents = catDict->lookup("OutputIntents");
    *acroForm = catDict->lookupNF("AcroForm").copy();
    *ocProperties = catDict->lookupNF("OCProperties").copy();
    *names = catDict->lookup("Names");

    Ref *firstPageRef = doc->getCatalog()->getPageRef(1);
    if (!firstPageRef) {
        error(errSyntaxError, -1, "Could not find the first page reference.");
        return false;
    }

    if (!acroForm->isNull()) {
        doc->markAcroForm(acroForm, yRef, countRef, 0, firstPageRef->num, firstPageRef->num);
    }
    if (!ocProperties->isNull() && ocProperties->isDict()) {
        doc->markPageObjects(ocProperties->getDict(), yRef, countRef, 0, firstPageRef->num, firstPageRef->num);
    }
    if (!names->isNull() && names->isDict()) {
        doc->markPageObjects(names->getDict(), yRef, countRef, 0, firstPageRef->num, firstPageRef->num);
    }
    if (intents->isArray() && intents->arrayGetLength() > 0) {
        for (int i = intents->arrayGetLength() - 1; i >= 0; --i) {
            Object intent = intents->arrayGet(i, 0);
            if (intent.isDict()) {
                doc->markPageObjects(intent.getDict(), yRef, countRef, 0, 0, 0);
            } else {
                intents->arrayRemove(i);
            }
        }
    }

    return true;
}

bool appendExistingPage(PDFDoc *doc, int pageNo, XRef *yRef, XRef *countRef, unsigned int numOffset, std::vector<PageEntry> *pages, bool resetAnnotationUniqueNames = false)
{
    Page *pageInfo = doc->getCatalog()->getPage(pageNo);
    if (!pageInfo) {
        error(errSyntaxError, -1, "Could not read page {0:d}.", pageNo);
        return false;
    }

    const PDFRectangle *cropBox = pageInfo->isCropped() ? pageInfo->getCropBox() : nullptr;
    if (!doc->replacePageDict(pageNo, pageInfo->getRotate(), pageInfo->getMediaBox(), cropBox)) {
        error(errSyntaxError, -1, "PDFDoc::replacePageDict failed for page {0:d}.", pageNo);
        return false;
    }

    Ref *refPage = doc->getCatalog()->getPageRef(pageNo);
    if (!refPage) {
        error(errSyntaxError, -1, "Could not find page reference for page {0:d}.", pageNo);
        return false;
    }

    Object page = doc->getXRef()->fetch(*refPage);
    if (!page.isDict()) {
        error(errSyntaxError, -1, "Page object for page {0:d} is not a dictionary.", pageNo);
        return false;
    }

    Dict *pageDict = page.getDict();
    Object *resources = pageInfo->getResourceDictObject();
    if (resources->isDict()) {
        pageDict->set("Resources", resources->copy());
    }

    if (!doc->markPageObjects(pageDict, yRef, countRef, numOffset, refPage->num, refPage->num)) {
        error(errSyntaxError, -1, "PDFDoc::markPageObjects failed for page {0:d}.", pageNo);
        return false;
    }

    Object annotsObj = pageDict->lookupNF("Annots").copy();
    if (!annotsObj.isNull()) {
        Object annots = annotsObj.fetch(doc->getXRef());
        if (annots.isArray()) {
            Array *array = annots.getArray();
            for (int i = 0; i < array->getLength(); ++i) {
                Object annot = array->get(i);
                if (!annot.isDict()) {
                    continue;
                }
                Dict *annotDict = annot.getDict();
                Object type = annotDict->lookup("Type");
                Object subType = annotDict->lookup("Subtype");
                if ((type.isName() && std::strcmp(type.getName(), "Annot") == 0) || !subType.isNull()) {
                    annotDict->remove("P");
                    if (resetAnnotationUniqueNames) {
                        annotDict->remove("NM");
                    }
                    Object annotRef = array->getNF(i).copy();
                    if (annotRef.isRef()) {
                        doc->getXRef()->setModifiedObject(&annot, annotRef.getRef());
                    }
                }
            }
        }
        doc->markAnnotations(&annotsObj, yRef, countRef, numOffset, refPage->num, refPage->num);
    }

    pages->push_back(PageEntry { std::move(page), numOffset });
    return true;
}

bool appendExistingPage(PDFDoc *doc, int pageNo, XRef *yRef, XRef *countRef, std::vector<PageEntry> *pages)
{
    return appendExistingPage(doc, pageNo, yRef, countRef, 0, pages);
}

bool appendBlankPageLike(PDFDoc *doc, int referencePageNo, XRef *xref, std::vector<PageEntry> *pages)
{
    Page *pageInfo = doc->getCatalog()->getPage(referencePageNo);
    if (!pageInfo) {
        error(errSyntaxError, -1, "Could not read reference page {0:d}.", referencePageNo);
        return false;
    }

    const PDFRectangle *cropBox = pageInfo->isCropped() ? pageInfo->getCropBox() : nullptr;
    pages->push_back(PageEntry { makeBlankPage(xref, pageInfo->getMediaBox(), cropBox, pageInfo->getRotate()), 0 });
    return true;
}

bool appendBlankPageWithSize(double width, double height, XRef *xref, std::vector<PageEntry> *pages)
{
    if (width <= 0 || height <= 0) {
        error(errCommandLine, -1, "Blank page width and height must be positive.");
        return false;
    }

    const PDFRectangle mediaBox(0, 0, width, height);
    pages->push_back(PageEntry { makeBlankPage(xref, &mediaBox, nullptr, 0), 0 });
    return true;
}

struct PageSequenceEdit
{
    int insertBlankAfter = -1;
    double blankWidth = 0;
    double blankHeight = 0;
    int insertPdfPageAfter = -1;
    std::string insertPdfFileName;
    int insertPdfPage = -1;
    int deletePage = -1;
    int movePageFrom = -1;
    int movePageTo = -1;
    std::vector<int> pageOrder;
    int rotatePage = -1;
    int rotationDegrees = -1;
};

PdfPageSequenceEditor::Result writePageSequence(const std::string &inputFileName, const std::string &outputFileName, PageSequenceEdit edit)
{
    if (inputFileName.empty() || outputFileName.empty()) {
        return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "Input and output file names must not be empty.");
    }
    if (inputFileName == outputFileName) {
        return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "Input and output files must be different.");
    }

    ensureGlobalParams();

    auto doc = std::make_unique<PDFDoc>(std::make_unique<GooString>(inputFileName));
    if (!doc->isOk() || !doc->getXRef()->getCatalog().isDict()) {
        error(errSyntaxError, -1, "Could not edit damaged file ('{0:s}').", inputFileName.c_str());
        return makeError(PdfPageSequenceEditor::Error::DamagedInput, "Could not edit damaged input PDF.");
    }
    if (doc->isEncrypted()) {
        error(errUnimplemented, -1, "Could not edit encrypted file ('{0:s}').", inputFileName.c_str());
        return makeError(PdfPageSequenceEditor::Error::EncryptedInput, "Could not edit encrypted input PDF.");
    }

    const int pageCount = doc->getNumPages();
    if (pageCount < 1) {
        error(errSyntaxError, -1, "The input PDF has no pages.");
        return makeError(PdfPageSequenceEditor::Error::DamagedInput, "The input PDF has no pages.");
    }

    const bool wantsInsert = edit.insertBlankAfter >= 0;
    const bool wantsInsertPdfPage = edit.insertPdfPageAfter >= 0;
    const bool wantsDelete = edit.deletePage >= 0;
    const bool wantsMove = edit.movePageFrom >= 0 || edit.movePageTo >= 0;
    const bool wantsReorder = !edit.pageOrder.empty();
    const bool wantsRotate = edit.rotatePage >= 0;
    if (static_cast<int>(wantsInsert) + static_cast<int>(wantsInsertPdfPage) + static_cast<int>(wantsDelete) + static_cast<int>(wantsMove) + static_cast<int>(wantsReorder) + static_cast<int>(wantsRotate) != 1) {
        error(errCommandLine, -1, "Exactly one page edit operation must be specified.");
        return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "Exactly one page edit operation must be specified.", pageCount);
    }
    if (edit.insertBlankAfter >= 0 && (edit.insertBlankAfter > pageCount)) {
        error(errCommandLine, -1, "The insertion point must be between 0 and {0:d}.", pageCount);
        return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "The insertion point is outside the document page range.", pageCount);
    }
    if (edit.insertBlankAfter >= 0 && (edit.blankWidth < 0 || edit.blankHeight < 0 || (edit.blankWidth == 0 && edit.blankHeight > 0) || (edit.blankWidth > 0 && edit.blankHeight == 0))) {
        error(errCommandLine, -1, "Both blank page width and height must be specified, or neither.");
        return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "Both blank page width and height must be specified, or neither.", pageCount);
    }
    if (edit.insertPdfPageAfter >= 0 && (edit.insertPdfPageAfter > pageCount || edit.insertPdfFileName.empty() || edit.insertPdfPage < 1)) {
        error(errCommandLine, -1, "The imported page insertion arguments are invalid.");
        return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "The imported page insertion arguments are invalid.", pageCount);
    }
    if (edit.deletePage >= 0 && (edit.deletePage < 1 || edit.deletePage > pageCount)) {
        error(errCommandLine, -1, "The page to delete must be between 1 and {0:d}.", pageCount);
        return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "The page to delete is outside the document page range.", pageCount);
    }
    if (edit.deletePage >= 0 && pageCount == 1) {
        error(errCommandLine, -1, "The only page in the document cannot be deleted.");
        return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "The only page in the document cannot be deleted.", pageCount);
    }
    if (wantsMove && (edit.movePageFrom < 1 || edit.movePageFrom > pageCount || edit.movePageTo < 1 || edit.movePageTo > pageCount)) {
        error(errCommandLine, -1, "The page move source and destination must be between 1 and {0:d}.", pageCount);
        return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "The page move source or destination is outside the document page range.", pageCount);
    }
    if (wantsRotate && (edit.rotatePage < 1 || edit.rotatePage > pageCount)) {
        error(errCommandLine, -1, "The page to rotate must be between 1 and {0:d}.", pageCount);
        return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "The page to rotate is outside the document page range.", pageCount);
    }
    if (wantsRotate && (edit.rotationDegrees < 0 || edit.rotationDegrees >= 360 || edit.rotationDegrees % 90 != 0)) {
        error(errCommandLine, -1, "The page rotation must be 0, 90, 180, or 270 degrees.");
        return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "The page rotation is invalid.", pageCount);
    }
    if (wantsReorder) {
        if (static_cast<int>(edit.pageOrder.size()) != pageCount) {
            error(errCommandLine, -1, "The page order must contain exactly {0:d} pages.", pageCount);
            return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "The page order length does not match the document page count.", pageCount);
        }
        std::vector<bool> seen(pageCount + 1, false);
        for (int pageNo : edit.pageOrder) {
            if (pageNo < 1 || pageNo > pageCount || seen[pageNo]) {
                error(errCommandLine, -1, "The page order must be a permutation of 1 through {0:d}.", pageCount);
                return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "The page order is not a valid page permutation.", pageCount);
            }
            seen[pageNo] = true;
        }
    }

    std::unique_ptr<PDFDoc> insertedDoc;
    int insertedDocPageCount = 0;
    if (wantsInsertPdfPage) {
        insertedDoc = std::make_unique<PDFDoc>(std::make_unique<GooString>(edit.insertPdfFileName));
        if (!insertedDoc->isOk() || !insertedDoc->getXRef()->getCatalog().isDict()) {
            error(errSyntaxError, -1, "Could not edit damaged imported file ('{0:s}').", edit.insertPdfFileName.c_str());
            return makeError(PdfPageSequenceEditor::Error::DamagedInput, "Could not read imported PDF page.", pageCount);
        }
        if (insertedDoc->isEncrypted()) {
            error(errUnimplemented, -1, "Could not edit encrypted imported file ('{0:s}').", edit.insertPdfFileName.c_str());
            return makeError(PdfPageSequenceEditor::Error::EncryptedInput, "Could not import a page from an encrypted PDF.", pageCount);
        }
        insertedDocPageCount = insertedDoc->getNumPages();
        if (edit.insertPdfPage > insertedDocPageCount) {
            error(errCommandLine, -1, "The imported page number must be between 1 and {0:d}.", insertedDocPageCount);
            return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "The imported page is outside the source PDF page range.", pageCount);
        }
    }

    FILE *file = std::fopen(outputFileName.c_str(), "wb");
    if (!file) {
        error(errIO, -1, "Could not open file '{0:s}'.", outputFileName.c_str());
        return makeError(PdfPageSequenceEditor::Error::IoError, "Could not open output file.", pageCount);
    }

    auto *outStr = new FileOutStream(file, 0);
    auto *yRef = new XRef();
    auto *countRef = new XRef();
    yRef->add(0, 65535, 0, false);

    int majorVersion = doc->getPDFMajorVersion();
    int minorVersion = doc->getPDFMinorVersion();
    if (insertedDoc && (insertedDoc->getPDFMajorVersion() > majorVersion || (insertedDoc->getPDFMajorVersion() == majorVersion && insertedDoc->getPDFMinorVersion() > minorVersion))) {
        majorVersion = insertedDoc->getPDFMajorVersion();
        minorVersion = insertedDoc->getPDFMinorVersion();
    }
    PDFDoc::writeHeader(outStr, majorVersion, minorVersion);

    Object intents;
    Object acroForm;
    Object ocProperties;
    Object names;
    std::vector<PageEntry> pages;
    std::vector<int> pageOrder;
    pageOrder.reserve(pageCount);
    if (wantsReorder) {
        pageOrder = std::move(edit.pageOrder);
    } else if (wantsMove) {
        for (int pageNo = 1; pageNo <= pageCount; ++pageNo) {
            if (pageNo != edit.movePageFrom) {
                pageOrder.push_back(pageNo);
            }
        }
        pageOrder.insert(pageOrder.begin() + (edit.movePageTo - 1), edit.movePageFrom);
    } else {
        for (int pageNo = 1; pageNo <= pageCount; ++pageNo) {
            pageOrder.push_back(pageNo);
        }
    }

    bool ok = markCatalogObjects(doc.get(), yRef, countRef, &intents, &acroForm, &ocProperties, &names);

    if (ok && edit.insertBlankAfter == 0) {
        ok = edit.blankWidth > 0 ? appendBlankPageWithSize(edit.blankWidth, edit.blankHeight, yRef, &pages) : appendBlankPageLike(doc.get(), 1, yRef, &pages);
    }

    int insertPdfPageIndex = wantsInsertPdfPage && edit.insertPdfPageAfter == 0 ? 0 : -1;

    for (int pageNo : pageOrder) {
        if (!ok) {
            break;
        }
        if (pageNo != edit.deletePage) {
            ok = appendExistingPage(doc.get(), pageNo, yRef, countRef, &pages);
        }
        if (ok && pageNo == edit.rotatePage) {
            pages.back().page.getDict()->set("Rotate", Object(edit.rotationDegrees));
        }
        if (ok && pageNo == edit.insertBlankAfter) {
            ok = edit.blankWidth > 0 ? appendBlankPageWithSize(edit.blankWidth, edit.blankHeight, yRef, &pages) : appendBlankPageLike(doc.get(), pageNo, yRef, &pages);
        }
        if (ok && pageNo == edit.insertPdfPageAfter) {
            insertPdfPageIndex = static_cast<int>(pages.size());
        }
    }

    if (ok) {
        doc->writePageObjects(outStr, yRef, 0, true);

        if (insertedDoc && insertPdfPageIndex >= 0) {
            const unsigned int numOffset = yRef->getNumObjects() + 1;
            std::vector<PageEntry> insertedPages;
            ok = appendExistingPage(insertedDoc.get(), edit.insertPdfPage, yRef, countRef, numOffset, &insertedPages, true);
            if (ok) {
                insertedDoc->writePageObjects(outStr, yRef, numOffset, true);
                pages.insert(pages.begin() + insertPdfPageIndex, std::move(insertedPages.front()));
            }
        }

        const int rootNum = yRef->getNumObjects() + 1;
        yRef->add(rootNum, 0, outStr->getPos(), true);
        outStr->printf("%d 0 obj\n", rootNum);
        outStr->printf("<< /Type /Catalog /Pages %d 0 R", rootNum + 1);
        if (intents.isArray() && intents.arrayGetLength() > 0) {
            outStr->printf(" /OutputIntents [");
            for (int i = 0; i < intents.arrayGetLength(); ++i) {
                Object intent = intents.arrayGet(i, 0);
                if (intent.isDict()) {
                    PDFDoc::writeObject(&intent, outStr, yRef, 0, nullptr, cryptRC4, 0, 0, 0);
                }
            }
            outStr->printf("]");
        }
        if (!acroForm.isNull()) {
            outStr->printf(" /AcroForm ");
            PDFDoc::writeObject(&acroForm, outStr, yRef, 0, nullptr, cryptRC4, 0, 0, 0);
        }
        if (!ocProperties.isNull() && ocProperties.isDict()) {
            outStr->printf(" /OCProperties ");
            PDFDoc::writeObject(&ocProperties, outStr, yRef, 0, nullptr, cryptRC4, 0, 0, 0);
        }
        if (!names.isNull() && names.isDict()) {
            outStr->printf(" /Names ");
            PDFDoc::writeObject(&names, outStr, yRef, 0, nullptr, cryptRC4, 0, 0, 0);
        }
        outStr->printf(">>\nendobj\n");

        yRef->add(rootNum + 1, 0, outStr->getPos(), true);
        outStr->printf("%d 0 obj\n", rootNum + 1);
        outStr->printf("<< /Type /Pages /Kids [");
        for (std::size_t i = 0; i < pages.size(); ++i) {
            outStr->printf(" %zu 0 R", rootNum + i + 2);
        }
        outStr->printf(" ] /Count %zu >>\nendobj\n", pages.size());

        for (std::size_t i = 0; i < pages.size(); ++i) {
            yRef->add(rootNum + static_cast<int>(i) + 2, 0, outStr->getPos(), true);
            outStr->printf("%zu 0 obj\n", rootNum + i + 2);
            outStr->printf("<< ");
            Dict *pageDict = pages[i].page.getDict();
            for (int keyIndex = 0; keyIndex < pageDict->getLength(); ++keyIndex) {
                if (keyIndex > 0) {
                    outStr->printf(" ");
                }
                const char *key = pageDict->getKey(keyIndex);
                Object value = pageDict->getValNF(keyIndex).copy();
                if (std::strcmp(key, "Parent") == 0) {
                    outStr->printf("/Parent %d 0 R", rootNum + 1);
                } else {
                    outStr->printf("/%s ", key);
                    PDFDoc::writeObject(&value, outStr, yRef, pages[i].numOffset, nullptr, cryptRC4, 0, 0, 0);
                }
            }
            outStr->printf(" >>\nendobj\n");
        }

        const Goffset xrefOffset = outStr->getPos();
        Ref rootRef;
        rootRef.num = rootNum;
        rootRef.gen = 0;
        const int xrefSize = yRef->getNumObjects();
        Object trailerDict = PDFDoc::createTrailerDict(xrefSize, false, 0, &rootRef, yRef, outputFileName.c_str(), outStr->getPos());
        PDFDoc::writeXRefTableTrailer(std::move(trailerDict), yRef, true, xrefOffset, outStr, yRef);
    }

    outStr->close();
    delete outStr;
    std::fclose(file);
    delete yRef;
    delete countRef;

    if (!ok) {
        return makeError(PdfPageSequenceEditor::Error::WriteError, "Failed while writing the edited page sequence.", pageCount);
    }

    return PdfPageSequenceEditor::Result { PdfPageSequenceEditor::Error::None, std::string(), pageCount, static_cast<int>(pages.size()) };
}

}

namespace PdfPageSequenceEditor
{

Result insertBlankPageAfter(const std::string &inputFileName, const std::string &outputFileName, int pageNumber)
{
    PageSequenceEdit edit;
    edit.insertBlankAfter = pageNumber;
    return writePageSequence(inputFileName, outputFileName, std::move(edit));
}

Result insertBlankPageAfter(const std::string &inputFileName, const std::string &outputFileName, int pageNumber, double width, double height)
{
    PageSequenceEdit edit;
    edit.insertBlankAfter = pageNumber;
    edit.blankWidth = width;
    edit.blankHeight = height;
    return writePageSequence(inputFileName, outputFileName, std::move(edit));
}

Result insertPdfPageAfter(const std::string &inputFileName, const std::string &outputFileName, int pageNumber, const std::string &insertedFileName, int pageToInsert)
{
    PageSequenceEdit edit;
    edit.insertPdfPageAfter = pageNumber;
    edit.insertPdfFileName = insertedFileName;
    edit.insertPdfPage = pageToInsert;
    return writePageSequence(inputFileName, outputFileName, std::move(edit));
}

Result deletePage(const std::string &inputFileName, const std::string &outputFileName, int pageNumber)
{
    PageSequenceEdit edit;
    edit.deletePage = pageNumber;
    return writePageSequence(inputFileName, outputFileName, std::move(edit));
}

Result movePage(const std::string &inputFileName, const std::string &outputFileName, int sourcePageNumber, int destinationPageNumber)
{
    PageSequenceEdit edit;
    edit.movePageFrom = sourcePageNumber;
    edit.movePageTo = destinationPageNumber;
    return writePageSequence(inputFileName, outputFileName, std::move(edit));
}

Result reorderPages(const std::string &inputFileName, const std::string &outputFileName, const std::vector<int> &pageOrder)
{
    PageSequenceEdit edit;
    edit.pageOrder = pageOrder;
    return writePageSequence(inputFileName, outputFileName, std::move(edit));
}

Result rotatePage(const std::string &inputFileName, const std::string &outputFileName, int pageNumber, int rotationDegrees)
{
    PageSequenceEdit edit;
    edit.rotatePage = pageNumber;
    edit.rotationDegrees = rotationDegrees;
    return writePageSequence(inputFileName, outputFileName, std::move(edit));
}

}
