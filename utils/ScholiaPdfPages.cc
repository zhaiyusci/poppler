//========================================================================
//
// ScholiaPdfPages.cc
//
// This file is licensed under the GPLv2 or later
//
// A small page-sequence editing library for Scholia.  This intentionally
// lives next to pdfunite instead of changing Poppler's core ABI.
//
//========================================================================

#include "ScholiaPdfPages.h"

#include "config.h"
#include <poppler-config.h>

#include <cstdio>
#include <cstring>
#include <memory>
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

ScholiaPdfPages::Result makeError(ScholiaPdfPages::Error error, std::string message, int inputPageCount = 0)
{
    return ScholiaPdfPages::Result { error, std::move(message), inputPageCount, 0 };
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

bool appendExistingPage(PDFDoc *doc, int pageNo, XRef *yRef, XRef *countRef, std::vector<PageEntry> *pages)
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

    if (!doc->markPageObjects(pageDict, yRef, countRef, 0, refPage->num, refPage->num)) {
        error(errSyntaxError, -1, "PDFDoc::markPageObjects failed for page {0:d}.", pageNo);
        return false;
    }

    Object annotsObj = pageDict->lookupNF("Annots").copy();
    if (!annotsObj.isNull()) {
        doc->markAnnotations(&annotsObj, yRef, countRef, 0, refPage->num, refPage->num);
    }

    pages->push_back(PageEntry { std::move(page), 0 });
    return true;
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

ScholiaPdfPages::Result writePageSequence(const std::string &inputFileName, const std::string &outputFileName, int insertBlankAfter)
{
    if (inputFileName.empty() || outputFileName.empty()) {
        return makeError(ScholiaPdfPages::Error::InvalidArguments, "Input and output file names must not be empty.");
    }
    if (inputFileName == outputFileName) {
        return makeError(ScholiaPdfPages::Error::InvalidArguments, "Input and output files must be different.");
    }

    ensureGlobalParams();

    auto doc = std::make_unique<PDFDoc>(std::make_unique<GooString>(inputFileName));
    if (!doc->isOk() || !doc->getXRef()->getCatalog().isDict()) {
        error(errSyntaxError, -1, "Could not edit damaged file ('{0:s}').", inputFileName.c_str());
        return makeError(ScholiaPdfPages::Error::DamagedInput, "Could not edit damaged input PDF.");
    }
    if (doc->isEncrypted()) {
        error(errUnimplemented, -1, "Could not edit encrypted file ('{0:s}').", inputFileName.c_str());
        return makeError(ScholiaPdfPages::Error::EncryptedInput, "Could not edit encrypted input PDF.");
    }

    const int pageCount = doc->getNumPages();
    if (pageCount < 1) {
        error(errSyntaxError, -1, "The input PDF has no pages.");
        return makeError(ScholiaPdfPages::Error::DamagedInput, "The input PDF has no pages.");
    }
    if (insertBlankAfter < 0 || insertBlankAfter > pageCount) {
        error(errCommandLine, -1, "The insertion point must be between 0 and {0:d}.", pageCount);
        return makeError(ScholiaPdfPages::Error::InvalidArguments, "The insertion point is outside the document page range.", pageCount);
    }

    FILE *file = std::fopen(outputFileName.c_str(), "wb");
    if (!file) {
        error(errIO, -1, "Could not open file '{0:s}'.", outputFileName.c_str());
        return makeError(ScholiaPdfPages::Error::IoError, "Could not open output file.", pageCount);
    }

    auto *outStr = new FileOutStream(file, 0);
    auto *yRef = new XRef();
    auto *countRef = new XRef();
    yRef->add(0, 65535, 0, false);

    const int majorVersion = doc->getPDFMajorVersion();
    const int minorVersion = doc->getPDFMinorVersion();
    PDFDoc::writeHeader(outStr, majorVersion, minorVersion);

    Object intents;
    Object acroForm;
    Object ocProperties;
    Object names;
    std::vector<PageEntry> pages;
    bool ok = markCatalogObjects(doc.get(), yRef, countRef, &intents, &acroForm, &ocProperties, &names);

    if (ok && insertBlankAfter == 0) {
        ok = appendBlankPageLike(doc.get(), 1, yRef, &pages);
    }

    for (int pageNo = 1; ok && pageNo <= pageCount; ++pageNo) {
        ok = appendExistingPage(doc.get(), pageNo, yRef, countRef, &pages);
        if (ok && pageNo == insertBlankAfter) {
            ok = appendBlankPageLike(doc.get(), pageNo, yRef, &pages);
        }
    }

    int objectsCount = 0;
    if (ok) {
        objectsCount += doc->writePageObjects(outStr, yRef, 0, true);

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
        objectsCount++;

        yRef->add(rootNum + 1, 0, outStr->getPos(), true);
        outStr->printf("%d 0 obj\n", rootNum + 1);
        outStr->printf("<< /Type /Pages /Kids [");
        for (std::size_t i = 0; i < pages.size(); ++i) {
            outStr->printf(" %zu 0 R", rootNum + i + 2);
        }
        outStr->printf(" ] /Count %zu >>\nendobj\n", pages.size());
        objectsCount++;

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
            objectsCount++;
        }

        const Goffset xrefOffset = outStr->getPos();
        Ref rootRef;
        rootRef.num = rootNum;
        rootRef.gen = 0;
        Object trailerDict = PDFDoc::createTrailerDict(objectsCount, false, 0, &rootRef, yRef, outputFileName.c_str(), outStr->getPos());
        PDFDoc::writeXRefTableTrailer(std::move(trailerDict), yRef, true, xrefOffset, outStr, yRef);
    }

    outStr->close();
    delete outStr;
    std::fclose(file);
    delete yRef;
    delete countRef;

    if (!ok) {
        return makeError(ScholiaPdfPages::Error::WriteError, "Failed while writing the edited page sequence.", pageCount);
    }

    return ScholiaPdfPages::Result { ScholiaPdfPages::Error::None, std::string(), pageCount, static_cast<int>(pages.size()) };
}

}

namespace ScholiaPdfPages
{

Result insertBlankPageAfter(const std::string &inputFileName, const std::string &outputFileName, int pageNumber)
{
    return writePageSequence(inputFileName, outputFileName, pageNumber);
}

}
