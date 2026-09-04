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
#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <GlobalParams.h>
#include <PDFDoc.h>

#include "Array.h"
#include "Catalog.h"
#include "Dict.h"
#include "Error.h"
#include "Link.h"
#include "Object.h"
#include "Page.h"

namespace
{

struct PageEntry
{
    Object page;
    unsigned int numOffset = 0;
    Ref sourcePageRef { .num = -1, .gen = 0 };
};

bool markPageReference(PDFDoc *doc, const Ref &pageRef, XRef *outputXRef, unsigned int numOffset)
{
    const int outputNum = pageRef.num + static_cast<int>(numOffset);
    if (outputNum < outputXRef->getNumObjects() && outputXRef->getEntry(outputNum)->type != xrefEntryFree) {
        return true;
    }

    const XRefEntry *sourceEntry = doc->getXRef()->getEntry(pageRef.num);
    if (!sourceEntry || sourceEntry->type == xrefEntryFree || !outputXRef->add(outputNum, pageRef.gen, 0, true)) {
        return false;
    }
    if (sourceEntry->type == xrefEntryCompressed) {
        outputXRef->getEntry(outputNum)->type = xrefEntryCompressed;
    }
    return true;
}

void addDestinationCoordinate(Array *array, bool shouldChange, double value)
{
    array->add(shouldChange ? Object(value) : Object::null());
}

Object makeExplicitDestination(XRef *xref, const LinkDest &destination, const Ref &pageRef)
{
    auto *array = new Array(xref);
    array->add(Object(pageRef));
    switch (destination.getKind()) {
    case destXYZ:
        array->add(Object(objName, "XYZ"));
        addDestinationCoordinate(array, destination.getChangeLeft(), destination.getLeft());
        addDestinationCoordinate(array, destination.getChangeTop(), destination.getTop());
        addDestinationCoordinate(array, destination.getChangeZoom(), destination.getZoom());
        break;
    case destFit:
        array->add(Object(objName, "Fit"));
        break;
    case destFitH:
        array->add(Object(objName, "FitH"));
        addDestinationCoordinate(array, destination.getChangeTop(), destination.getTop());
        break;
    case destFitV:
        array->add(Object(objName, "FitV"));
        addDestinationCoordinate(array, destination.getChangeLeft(), destination.getLeft());
        break;
    case destFitR:
        array->add(Object(objName, "FitR"));
        array->add(Object(destination.getLeft()));
        array->add(Object(destination.getBottom()));
        array->add(Object(destination.getRight()));
        array->add(Object(destination.getTop()));
        break;
    case destFitB:
        array->add(Object(objName, "FitB"));
        break;
    case destFitBH:
        array->add(Object(objName, "FitBH"));
        addDestinationCoordinate(array, destination.getChangeTop(), destination.getTop());
        break;
    case destFitBV:
        array->add(Object(objName, "FitBV"));
        addDestinationCoordinate(array, destination.getChangeLeft(), destination.getLeft());
        break;
    }
    return Object(array);
}

using DestinationNameMap = std::map<std::string, std::string>;

struct NamedDestinationEntry
{
    std::string name;
    Object destination;
};

std::optional<Ref> destinationPageRef(PDFDoc *doc, const LinkDest &destination)
{
    if (destination.isPageRef()) {
        return destination.getPageRef();
    }

    Ref *pageRef = doc->getCatalog()->getPageRef(destination.getPageNum());
    if (!pageRef) {
        return std::nullopt;
    }
    return *pageRef;
}

bool destinationPointsToPage(const LinkDest &destination, int pageNo, const Ref &pageRef)
{
    return destination.isPageRef() ? destination.getPageRef() == pageRef : destination.getPageNum() == pageNo;
}

std::string commonDestinationSuffix(const std::vector<std::string> &sourceNames, const std::set<std::string> &usedNames, int preferredSuffix)
{
    for (int suffix = std::max(1, preferredSuffix);; ++suffix) {
        const std::string suffixText = "~" + std::to_string(suffix);
        const bool hasConflict = std::ranges::any_of(sourceNames, [&](const std::string &sourceName) {
            return usedNames.contains(sourceName + suffixText);
        });
        if (!hasConflict) {
            return suffixText;
        }
    }
}

void addCatalogDestinationNames(Catalog *catalog, std::set<std::string> *names)
{
    for (int i = 0; i < catalog->numDests(); ++i) {
        const char *name = catalog->getDestsName(i);
        if (name) {
            names->insert(name);
        }
    }
    for (int i = 0; i < catalog->numDestNameTree(); ++i) {
        const GooString *name = catalog->getDestNameTreeName(i);
        if (name) {
            names->insert(name->toStr());
        }
    }
}

std::vector<NamedDestinationEntry> materializeNameTreeDestinations(PDFDoc *doc)
{
    std::vector<NamedDestinationEntry> entries;
    Catalog *catalog = doc->getCatalog();
    entries.reserve(catalog->numDestNameTree());
    for (int i = 0; i < catalog->numDestNameTree(); ++i) {
        const GooString *name = catalog->getDestNameTreeName(i);
        std::unique_ptr<LinkDest> destination = catalog->getDestNameTreeDest(i);
        if (!name || !destination || !destination->isOk()) {
            continue;
        }
        const std::optional<Ref> pageRef = destinationPageRef(doc, *destination);
        if (!pageRef) {
            continue;
        }
        entries.push_back(NamedDestinationEntry { name->toStr(), makeExplicitDestination(doc->getXRef(), *destination, *pageRef) });
    }
    return entries;
}

void installDestinationNameTree(PDFDoc *hostDoc, Object *names, std::vector<NamedDestinationEntry> entries)
{
    std::ranges::sort(entries, {}, &NamedDestinationEntry::name);

    if (!names->isDict()) {
        *names = Object(new Dict(hostDoc->getXRef()));
    }
    auto *destinationTree = new Dict(hostDoc->getXRef());
    auto *destinationArray = new Array(hostDoc->getXRef());
    for (NamedDestinationEntry &entry : entries) {
        destinationArray->add(Object(std::string(entry.name)));
        destinationArray->add(std::move(entry.destination));
    }
    destinationTree->add("Names", Object(destinationArray));
    names->getDict()->set("Dests", Object(destinationTree));
}

DestinationNameMap cloneNamedDestinations(PDFDoc *hostDoc,
                                          PDFDoc *sourceDoc,
                                          int sourcePageNo,
                                          const Ref &sourcePageRef,
                                          const Ref &outputPageRef,
                                          PdfPageSequenceEditor::NamedDestinationConflictPolicy conflictPolicy,
                                          Object *names,
                                          Object *legacyDests)
{
    std::set<std::string> usedNames;
    addCatalogDestinationNames(hostDoc->getCatalog(), &usedNames);

    DestinationNameMap clones;
    std::vector<std::pair<std::string, std::unique_ptr<LinkDest>>> sourceDestinations;
    std::vector<NamedDestinationEntry> clonedEntries;
    std::set<std::string> visitedSourceNames;
    auto considerDestination = [&](const std::string &sourceName, std::unique_ptr<LinkDest> destination) {
        if (!visitedSourceNames.insert(sourceName).second || !destination || !destination->isOk() || !destinationPointsToPage(*destination, sourcePageNo, sourcePageRef)) {
            return;
        }
        sourceDestinations.emplace_back(sourceName, std::move(destination));
    };

    Catalog *sourceCatalog = sourceDoc->getCatalog();
    for (int i = 0; i < sourceCatalog->numDests(); ++i) {
        const char *sourceName = sourceCatalog->getDestsName(i);
        if (sourceName) {
            considerDestination(sourceName, sourceCatalog->getDestsDest(i));
        }
    }
    for (int i = 0; i < sourceCatalog->numDestNameTree(); ++i) {
        const GooString *sourceName = sourceCatalog->getDestNameTreeName(i);
        if (sourceName) {
            considerDestination(sourceName->toStr(), sourceCatalog->getDestNameTreeDest(i));
        }
    }

    std::vector<std::string> sourceNames;
    sourceNames.reserve(sourceDestinations.size());
    for (const auto &[sourceName, destination] : sourceDestinations) {
        sourceNames.push_back(sourceName);
    }
    const std::string suffix = conflictPolicy == PdfPageSequenceEditor::NamedDestinationConflictPolicy::AddSuffixes ? commonDestinationSuffix(sourceNames, usedNames, 2) : std::string();
    for (auto &[sourceName, destination] : sourceDestinations) {
        const std::string cloneName = sourceName + suffix;
        clones.emplace(sourceName, cloneName);
        if (usedNames.contains(cloneName)) {
            continue;
        }
        usedNames.insert(cloneName);
        Object cloneDestination = makeExplicitDestination(hostDoc->getXRef(), *destination, outputPageRef);
        // Poppler checks the legacy /Dests dictionary before the name tree and
        // does not fall back if both structures exist. Mirror the clone there
        // when the host uses it so either representation resolves identically.
        if (legacyDests->isDict()) {
            legacyDests->getDict()->set(cloneName, cloneDestination.copy());
        }
        clonedEntries.push_back(NamedDestinationEntry { cloneName, std::move(cloneDestination) });
    }

    if (!clonedEntries.empty()) {
        std::vector<NamedDestinationEntry> outputEntries = materializeNameTreeDestinations(hostDoc);
        outputEntries.insert(outputEntries.end(), std::make_move_iterator(clonedEntries.begin()), std::make_move_iterator(clonedEntries.end()));
        installDestinationNameTree(hostDoc, names, std::move(outputEntries));
    }
    return clones;
}

DestinationNameMap cloneNamedDestinationsForPages(PDFDoc *hostDoc,
                                                  PDFDoc *sourceDoc,
                                                  const std::map<Ref, Ref> &pageRefs,
                                                  PdfPageSequenceEditor::NamedDestinationConflictPolicy conflictPolicy,
                                                  int preferredSuffix,
                                                  std::set<std::string> *usedNames,
                                                  std::vector<NamedDestinationEntry> *outputEntries,
                                                  Object *legacyDests)
{
    DestinationNameMap clones;
    std::vector<std::pair<std::string, std::pair<std::unique_ptr<LinkDest>, Ref>>> sourceDestinations;
    std::set<std::string> visitedSourceNames;
    auto considerDestination = [&](const std::string &sourceName, std::unique_ptr<LinkDest> destination) {
        if (!visitedSourceNames.insert(sourceName).second || !destination || !destination->isOk()) {
            return;
        }
        const std::optional<Ref> sourcePageRef = destinationPageRef(sourceDoc, *destination);
        if (!sourcePageRef) {
            return;
        }
        const auto outputPageRef = pageRefs.find(*sourcePageRef);
        if (outputPageRef == pageRefs.end()) {
            return;
        }
        sourceDestinations.emplace_back(sourceName, std::make_pair(std::move(destination), outputPageRef->second));
    };

    Catalog *sourceCatalog = sourceDoc->getCatalog();
    for (int i = 0; i < sourceCatalog->numDests(); ++i) {
        const char *sourceName = sourceCatalog->getDestsName(i);
        if (sourceName) {
            considerDestination(sourceName, sourceCatalog->getDestsDest(i));
        }
    }
    for (int i = 0; i < sourceCatalog->numDestNameTree(); ++i) {
        const GooString *sourceName = sourceCatalog->getDestNameTreeName(i);
        if (sourceName) {
            considerDestination(sourceName->toStr(), sourceCatalog->getDestNameTreeDest(i));
        }
    }

    std::vector<std::string> sourceNames;
    sourceNames.reserve(sourceDestinations.size());
    for (const auto &[sourceName, destinationAndPage] : sourceDestinations) {
        sourceNames.push_back(sourceName);
    }
    const std::string suffix = conflictPolicy == PdfPageSequenceEditor::NamedDestinationConflictPolicy::AddSuffixes ? commonDestinationSuffix(sourceNames, *usedNames, preferredSuffix) : std::string();
    for (auto &[sourceName, destinationAndPage] : sourceDestinations) {
        const std::string cloneName = sourceName + suffix;
        clones.emplace(sourceName, cloneName);
        if (usedNames->contains(cloneName)) {
            continue;
        }
        usedNames->insert(cloneName);
        Object cloneDestination = makeExplicitDestination(hostDoc->getXRef(), *destinationAndPage.first, destinationAndPage.second);
        if (legacyDests->isDict()) {
            legacyDests->getDict()->set(cloneName, cloneDestination.copy());
        }
        outputEntries->push_back(NamedDestinationEntry { cloneName, std::move(cloneDestination) });
    }
    return clones;
}

enum class CopiedDestinationDisposition { Absent, Keep, Rewritten };

CopiedDestinationDisposition rewriteCopiedDestination(Dict *container,
                                                       const char *key,
                                                       const DestinationNameMap &clones)
{
    Object destination = container->lookup(key);
    if (destination.isNull()) {
        return CopiedDestinationDisposition::Absent;
    }

    std::string sourceName;
    if (destination.isName()) {
        sourceName = destination.getName();
    } else if (destination.isString()) {
        sourceName = destination.getString()->toStr();
    } else if (destination.isArray()) {
        return CopiedDestinationDisposition::Keep;
    } else {
        return CopiedDestinationDisposition::Keep;
    }

    const auto clone = clones.find(sourceName);
    if (clone == clones.end()) {
        return CopiedDestinationDisposition::Keep;
    }
    container->set(key, Object(std::string(clone->second)));
    return CopiedDestinationDisposition::Rewritten;
}

void rewriteClonedNamedLinks(PDFDoc *doc,
                             Dict *annotation,
                             const DestinationNameMap &clones)
{
    Object actionRef = annotation->lookupNF("A").copy();
    Object action = actionRef.fetch(doc->getXRef());
    if (action.isDict()) {
        Object kind = action.getDict()->lookup("S");
        if (!kind.isName("GoTo")) {
            return;
        }
        const CopiedDestinationDisposition disposition = rewriteCopiedDestination(action.getDict(), "D", clones);
        if (disposition == CopiedDestinationDisposition::Absent) {
            return;
        }
        if (disposition != CopiedDestinationDisposition::Rewritten) {
            return;
        }
        if (actionRef.isRef()) {
            doc->getXRef()->setModifiedObject(&action, actionRef.getRef());
        } else {
            annotation->set("A", std::move(action));
        }
        return;
    }

    rewriteCopiedDestination(annotation, "Dest", clones);
}

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

bool markCatalogObjects(PDFDoc *doc, XRef *yRef, XRef *countRef, Object *intents, Object *acroForm, Object *ocProperties, Object *names, Object *dests)
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
    *dests = catDict->lookup("Dests");

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
    if (!dests->isNull() && dests->isDict()) {
        doc->markPageObjects(dests->getDict(), yRef, countRef, 0, firstPageRef->num, firstPageRef->num);
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

bool appendExistingPage(PDFDoc *doc,
                        int pageNo,
                        XRef *yRef,
                        XRef *countRef,
                        unsigned int numOffset,
                        std::vector<PageEntry> *pages,
                        const DestinationNameMap *clonedDestinations = nullptr)
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
                    if (clonedDestinations) {
                        rewriteClonedNamedLinks(doc, annotDict, *clonedDestinations);
                    }
                    annotDict->remove("P");
                    if (clonedDestinations) {
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

    pages->push_back(PageEntry { std::move(page), numOffset, *refPage });
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
    PdfPageSequenceEditor::NamedDestinationConflictPolicy destinationConflictPolicy = PdfPageSequenceEditor::NamedDestinationConflictPolicy::KeepNames;
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
    Object dests;
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

    bool ok = markCatalogObjects(doc.get(), yRef, countRef, &intents, &acroForm, &ocProperties, &names, &dests);

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

    // Internal destinations identify their target with the indirect Page object
    // reference, not its page number. Keep the host document's Page references
    // stable for every edit, including importing a page from another PDF. The
    // imported page and its dependent objects use a disjoint, rebased range.
    int rootNum = -1;
    if (ok) {
        for (const PageEntry &page : pages) {
            if (page.sourcePageRef.num >= 0 && !markPageReference(doc.get(), page.sourcePageRef, yRef, page.numOffset)) {
                error(errSyntaxError, -1, "Could not preserve page reference {0:d}.", page.sourcePageRef.num);
                ok = false;
                break;
            }
        }
        if (ok) {
            rootNum = yRef->getNumObjects() + 1;
            const Ref pagesRootRef { .num = rootNum + 1, .gen = 0 };
            for (PageEntry &page : pages) {
                if (page.sourcePageRef.num < 0) {
                    continue;
                }
                page.page.getDict()->set("Parent", Object(pagesRootRef));
                doc->getXRef()->setModifiedObject(&page.page, page.sourcePageRef);
            }
            doc->writePageObjects(outStr, yRef, 0, true);
        }
    }

    if (ok && insertedDoc && insertPdfPageIndex >= 0) {
        // rootNum and rootNum + 1 are reserved for the new Catalog and Pages
        // objects. Since valid indirect object numbers start at one, applying
        // this offset places every imported object after those reservations.
        const unsigned int numOffset = rootNum + 1;
        Ref *sourcePageRef = insertedDoc->getCatalog()->getPageRef(edit.insertPdfPage);
        if (!sourcePageRef) {
            error(errSyntaxError, -1, "Could not find imported page reference for page {0:d}.", edit.insertPdfPage);
            ok = false;
        }
        const Ref outputPageRef { .num = sourcePageRef ? sourcePageRef->num + static_cast<int>(numOffset) : -1, .gen = sourcePageRef ? sourcePageRef->gen : 0 };
        const DestinationNameMap clonedDestinations = ok ? cloneNamedDestinations(doc.get(), insertedDoc.get(), edit.insertPdfPage, *sourcePageRef, outputPageRef, edit.destinationConflictPolicy, &names, &dests) : DestinationNameMap {};
        std::vector<PageEntry> insertedPages;
        if (ok) {
            ok = appendExistingPage(insertedDoc.get(), edit.insertPdfPage, yRef, countRef, numOffset, &insertedPages, &clonedDestinations);
        }
        if (ok) {
            insertedDoc->writePageObjects(outStr, yRef, numOffset, true);
            pages.insert(pages.begin() + insertPdfPageIndex, std::move(insertedPages.front()));
        }
    }

    if (ok) {
        std::vector<Ref> outputPageRefs;
        outputPageRefs.reserve(pages.size());
        int nextGeneratedPageRef = rootNum + 2;
        for (const PageEntry &page : pages) {
            if (page.sourcePageRef.num >= 0) {
                outputPageRefs.push_back(Ref { .num = page.sourcePageRef.num + static_cast<int>(page.numOffset), .gen = page.sourcePageRef.gen });
            } else {
                outputPageRefs.push_back(Ref { .num = nextGeneratedPageRef++, .gen = 0 });
            }
        }

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
        if (!dests.isNull() && dests.isDict()) {
            outStr->printf(" /Dests ");
            PDFDoc::writeObject(&dests, outStr, yRef, 0, nullptr, cryptRC4, 0, 0, 0);
        }
        outStr->printf(">>\nendobj\n");

        yRef->add(rootNum + 1, 0, outStr->getPos(), true);
        outStr->printf("%d 0 obj\n", rootNum + 1);
        outStr->printf("<< /Type /Pages /Kids [");
        for (const Ref &pageRef : outputPageRefs) {
            outStr->printf(" %d %d R", pageRef.num, pageRef.gen);
        }
        outStr->printf(" ] /Count %zu >>\nendobj\n", pages.size());

        for (std::size_t i = 0; i < pages.size(); ++i) {
            if (pages[i].numOffset == 0 && pages[i].sourcePageRef.num >= 0) {
                continue;
            }
            const Ref pageRef = outputPageRefs[i];
            yRef->add(pageRef.num, pageRef.gen, outStr->getPos(), true);
            outStr->printf("%d %d obj\n", pageRef.num, pageRef.gen);
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

PdfPageSequenceEditor::Result combinePdfFilesImpl(const std::vector<std::string> &inputFileNames,
                                                  const std::string &outputFileName,
                                                  PdfPageSequenceEditor::NamedDestinationConflictPolicy conflictPolicy)
{
    if (inputFileNames.size() < 2 || outputFileName.empty()) {
        return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "At least two input PDFs and one output file are required.");
    }
    for (const std::string &inputFileName : inputFileNames) {
        if (inputFileName.empty() || inputFileName == outputFileName) {
            return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "Input PDFs must be non-empty and different from the output file.");
        }
    }

    ensureGlobalParams();

    std::vector<std::unique_ptr<PDFDoc>> documents;
    documents.reserve(inputFileNames.size());
    int totalPageCount = 0;
    int majorVersion = 1;
    int minorVersion = 0;
    for (const std::string &inputFileName : inputFileNames) {
        auto document = std::make_unique<PDFDoc>(std::make_unique<GooString>(inputFileName));
        if (!document->isOk() || !document->getXRef()->getCatalog().isDict() || document->getNumPages() < 1) {
            error(errSyntaxError, -1, "Could not combine damaged file ('{0:s}').", inputFileName.c_str());
            return makeError(PdfPageSequenceEditor::Error::DamagedInput, "Could not read one of the input PDFs.", totalPageCount);
        }
        if (document->isEncrypted()) {
            error(errUnimplemented, -1, "Could not combine encrypted file ('{0:s}').", inputFileName.c_str());
            return makeError(PdfPageSequenceEditor::Error::EncryptedInput, "Encrypted PDFs cannot be combined.", totalPageCount);
        }
        totalPageCount += document->getNumPages();
        if (document->getPDFMajorVersion() > majorVersion || (document->getPDFMajorVersion() == majorVersion && document->getPDFMinorVersion() > minorVersion)) {
            majorVersion = document->getPDFMajorVersion();
            minorVersion = document->getPDFMinorVersion();
        }
        documents.push_back(std::move(document));
    }

    FILE *file = std::fopen(outputFileName.c_str(), "wb");
    if (!file) {
        error(errIO, -1, "Could not open file '{0:s}'.", outputFileName.c_str());
        return makeError(PdfPageSequenceEditor::Error::IoError, "Could not open output file.", totalPageCount);
    }

    auto *outStr = new FileOutStream(file, 0);
    auto *yRef = new XRef();
    auto *countRef = new XRef();
    yRef->add(0, 65535, 0, false);
    PDFDoc::writeHeader(outStr, majorVersion, minorVersion);

    PDFDoc *hostDoc = documents.front().get();
    Object intents;
    Object acroForm;
    Object ocProperties;
    Object names;
    Object dests;
    std::vector<PageEntry> pages;
    pages.reserve(totalPageCount);

    bool ok = markCatalogObjects(hostDoc, yRef, countRef, &intents, &acroForm, &ocProperties, &names, &dests);
    std::set<std::string> usedDestinationNames;
    std::vector<NamedDestinationEntry> outputDestinations;
    DestinationNameMap hostDestinations;
    if (ok && conflictPolicy == PdfPageSequenceEditor::NamedDestinationConflictPolicy::AddSuffixes) {
        std::map<Ref, Ref> hostPageRefs;
        for (int pageNo = 1; pageNo <= hostDoc->getNumPages(); ++pageNo) {
            Ref *pageRef = hostDoc->getCatalog()->getPageRef(pageNo);
            if (!pageRef) {
                error(errSyntaxError, -1, "Could not find page reference {0:d} while preparing destination namespaces.", pageNo);
                ok = false;
                break;
            }
            hostPageRefs.emplace(*pageRef, *pageRef);
        }
        if (ok) {
            Object noLegacyDests = Object::null();
            hostDestinations = cloneNamedDestinationsForPages(hostDoc, hostDoc, hostPageRefs, conflictPolicy, 1, &usedDestinationNames, &outputDestinations, &noLegacyDests);
            dests = std::move(noLegacyDests);
        }
    } else if (ok) {
        addCatalogDestinationNames(hostDoc->getCatalog(), &usedDestinationNames);
        outputDestinations = materializeNameTreeDestinations(hostDoc);
    }
    const std::size_t originalDestinationCount = outputDestinations.size();

    for (int pageNo = 1; ok && pageNo <= hostDoc->getNumPages(); ++pageNo) {
        if (conflictPolicy == PdfPageSequenceEditor::NamedDestinationConflictPolicy::AddSuffixes) {
            ok = appendExistingPage(hostDoc, pageNo, yRef, countRef, 0, &pages, &hostDestinations);
        } else {
            ok = appendExistingPage(hostDoc, pageNo, yRef, countRef, &pages);
        }
    }

    int rootNum = -1;
    if (ok) {
        for (const PageEntry &page : pages) {
            if (!markPageReference(hostDoc, page.sourcePageRef, yRef, 0)) {
                error(errSyntaxError, -1, "Could not preserve page reference {0:d} while combining PDFs.", page.sourcePageRef.num);
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        rootNum = yRef->getNumObjects() + 1;
        const Ref pagesRootRef { .num = rootNum + 1, .gen = 0 };
        for (PageEntry &page : pages) {
            page.page.getDict()->set("Parent", Object(pagesRootRef));
            hostDoc->getXRef()->setModifiedObject(&page.page, page.sourcePageRef);
        }
        hostDoc->writePageObjects(outStr, yRef, 0, true);
    }

    unsigned int numOffset = rootNum + 1;
    for (std::size_t documentIndex = 1; ok && documentIndex < documents.size(); ++documentIndex) {
        PDFDoc *sourceDoc = documents[documentIndex].get();
        std::map<Ref, Ref> pageRefs;
        for (int pageNo = 1; pageNo <= sourceDoc->getNumPages(); ++pageNo) {
            Ref *sourcePageRef = sourceDoc->getCatalog()->getPageRef(pageNo);
            if (!sourcePageRef) {
                error(errSyntaxError, -1, "Could not find page reference {0:d} while combining PDFs.", pageNo);
                ok = false;
                break;
            }
            pageRefs.emplace(*sourcePageRef, Ref { .num = sourcePageRef->num + static_cast<int>(numOffset), .gen = sourcePageRef->gen });
        }
        if (!ok) {
            break;
        }

        const DestinationNameMap clonedDestinations = cloneNamedDestinationsForPages(hostDoc, sourceDoc, pageRefs, conflictPolicy, static_cast<int>(documentIndex) + 1, &usedDestinationNames, &outputDestinations, &dests);
        std::vector<PageEntry> sourcePages;
        sourcePages.reserve(sourceDoc->getNumPages());
        for (int pageNo = 1; ok && pageNo <= sourceDoc->getNumPages(); ++pageNo) {
            ok = appendExistingPage(sourceDoc, pageNo, yRef, countRef, numOffset, &sourcePages, &clonedDestinations);
        }
        if (!ok) {
            break;
        }

        const Ref pagesRootRef { .num = rootNum + 1, .gen = 0 };
        for (PageEntry &page : sourcePages) {
            page.page.getDict()->set("Parent", Object(pagesRootRef));
            sourceDoc->getXRef()->setModifiedObject(&page.page, page.sourcePageRef);
            if (!markPageReference(sourceDoc, page.sourcePageRef, yRef, numOffset)) {
                error(errSyntaxError, -1, "Could not import page reference {0:d} while combining PDFs.", page.sourcePageRef.num);
                ok = false;
                break;
            }
        }
        if (!ok) {
            break;
        }
        sourceDoc->writePageObjects(outStr, yRef, numOffset, true);
        pages.insert(pages.end(), std::make_move_iterator(sourcePages.begin()), std::make_move_iterator(sourcePages.end()));
        numOffset = yRef->getNumObjects();
    }

    if (ok && (conflictPolicy == PdfPageSequenceEditor::NamedDestinationConflictPolicy::AddSuffixes || outputDestinations.size() > originalDestinationCount)) {
        installDestinationNameTree(hostDoc, &names, std::move(outputDestinations));
    }

    if (ok) {
        std::vector<Ref> outputPageRefs;
        outputPageRefs.reserve(pages.size());
        for (const PageEntry &page : pages) {
            outputPageRefs.push_back(Ref { .num = page.sourcePageRef.num + static_cast<int>(page.numOffset), .gen = page.sourcePageRef.gen });
        }

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
        if (!dests.isNull() && dests.isDict()) {
            outStr->printf(" /Dests ");
            PDFDoc::writeObject(&dests, outStr, yRef, 0, nullptr, cryptRC4, 0, 0, 0);
        }
        outStr->printf(">>\nendobj\n");

        yRef->add(rootNum + 1, 0, outStr->getPos(), true);
        outStr->printf("%d 0 obj\n", rootNum + 1);
        outStr->printf("<< /Type /Pages /Kids [");
        for (const Ref &pageRef : outputPageRefs) {
            outStr->printf(" %d %d R", pageRef.num, pageRef.gen);
        }
        outStr->printf(" ] /Count %zu >>\nendobj\n", pages.size());

        for (std::size_t i = 0; i < pages.size(); ++i) {
            if (pages[i].numOffset == 0) {
                continue;
            }
            const Ref pageRef = outputPageRefs[i];
            yRef->add(pageRef.num, pageRef.gen, outStr->getPos(), true);
            outStr->printf("%d %d obj\n", pageRef.num, pageRef.gen);
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
        Ref rootRef { .num = rootNum, .gen = 0 };
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
        return makeError(PdfPageSequenceEditor::Error::WriteError, "Failed while combining the input PDFs.", totalPageCount);
    }
    return PdfPageSequenceEditor::Result { PdfPageSequenceEditor::Error::None, std::string(), totalPageCount, static_cast<int>(pages.size()) };
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

Result insertPdfPageAfter(const std::string &inputFileName,
                          const std::string &outputFileName,
                          int pageNumber,
                          const std::string &insertedFileName,
                          int pageToInsert,
                          NamedDestinationConflictPolicy conflictPolicy)
{
    PageSequenceEdit edit;
    edit.insertPdfPageAfter = pageNumber;
    edit.insertPdfFileName = insertedFileName;
    edit.insertPdfPage = pageToInsert;
    edit.destinationConflictPolicy = conflictPolicy;
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

Result combinePdfFiles(const std::vector<std::string> &inputFileNames, const std::string &outputFileName, NamedDestinationConflictPolicy conflictPolicy)
{
    return combinePdfFilesImpl(inputFileNames, outputFileName, conflictPolicy);
}

int pageCount(const std::string &inputFileName, std::string *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    if (inputFileName.empty()) {
        if (errorMessage) {
            *errorMessage = "The PDF file name must not be empty.";
        }
        return -1;
    }

    ensureGlobalParams();
    auto document = std::make_unique<PDFDoc>(std::make_unique<GooString>(inputFileName));
    if (!document->isOk() || !document->getXRef()->getCatalog().isDict() || document->getNumPages() < 1) {
        if (errorMessage) {
            *errorMessage = "Could not read the PDF.";
        }
        return -1;
    }
    if (document->isEncrypted()) {
        if (errorMessage) {
            *errorMessage = "Encrypted PDFs are not supported.";
        }
        return -1;
    }
    return document->getNumPages();
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
