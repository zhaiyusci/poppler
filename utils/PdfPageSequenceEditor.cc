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
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <set>
#include <string>
#include <vector>

#include <GlobalParams.h>
#include <PDFDoc.h>

#include "Array.h"
#include "Annot.h"
#include "Catalog.h"
#include "Dict.h"
#include "Error.h"
#include "GfxState.h"
#include "Link.h"
#include "Object.h"
#include "Page.h"
#include "Stream.h"
#include "XRef.h"

namespace PdfPageSequenceEditor {

class LivePageState
{
public:
    Ref pageRef = Ref::INVALID();
    std::vector<std::pair<std::string, Object>> namedDestinations;
    bool attached = false;
};

}

namespace {

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
        const bool hasConflict = std::ranges::any_of(sourceNames, [&](const std::string &sourceName) { return usedNames.contains(sourceName + suffixText); });
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
    std::ranges::sort(entries, { }, &NamedDestinationEntry::name);

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

DestinationNameMap cloneNamedDestinations(PDFDoc *hostDoc, PDFDoc *sourceDoc, int sourcePageNo, const Ref &sourcePageRef, const Ref &outputPageRef, PdfPageSequenceEditor::NamedDestinationConflictPolicy conflictPolicy, Object *names,
                                          Object *legacyDests, std::vector<std::pair<std::string, Object>> *addedDestinations = nullptr)
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
        if (addedDestinations) {
            addedDestinations->emplace_back(cloneName, cloneDestination.copy());
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

DestinationNameMap cloneNamedDestinationsForPages(PDFDoc *hostDoc, PDFDoc *sourceDoc, const std::map<Ref, Ref> &pageRefs, PdfPageSequenceEditor::NamedDestinationConflictPolicy conflictPolicy, int preferredSuffix,
                                                  std::set<std::string> *usedNames, std::vector<NamedDestinationEntry> *outputEntries, Object *legacyDests)
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

enum class CopiedDestinationDisposition
{
    Absent,
    Keep,
    Rewritten
};

CopiedDestinationDisposition rewriteCopiedDestination(Dict *container, const char *key, const DestinationNameMap &clones)
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

void rewriteClonedNamedLinks(PDFDoc *doc, Dict *annotation, const DestinationNameMap &clones)
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

bool rewriteNamedDestinationValue(Dict *container, const char *key, const std::string &oldName, const std::string &newName)
{
    const Object &destination = container->lookupNF(key);
    if (destination.isName() && destination.getName() == oldName) {
        container->set(key, Object(objName, newName.c_str()));
        return true;
    }
    if (destination.isString() && destination.getString()->toStr() == oldName) {
        container->set(key, Object(std::string(newName)));
        return true;
    }
    return false;
}

bool rewriteDirectNamedDestinationReferences(Object *object, const std::string &oldName, const std::string &newName)
{
    if (object->isDict()) {
        Dict *dict = object->getDict();
        bool changed = rewriteNamedDestinationValue(dict, "Dest", oldName, newName);
        const Object actionKind = dict->lookup("S");
        if (actionKind.isName("GoTo")) {
            changed = rewriteNamedDestinationValue(dict, "D", oldName, newName) || changed;
        }
        const Object type = dict->lookup("Type");
        if (type.isName("Catalog")) {
            changed = rewriteNamedDestinationValue(dict, "OpenAction", oldName, newName) || changed;
        }

        for (int i = 0; i < dict->getLength(); ++i) {
            const Object &child = dict->getValNF(i);
            if (!child.isRef()) {
                Object childCopy = child.copy();
                changed = rewriteDirectNamedDestinationReferences(&childCopy, oldName, newName) || changed;
            }
        }
        return changed;
    }
    if (object->isArray()) {
        Array *array = object->getArray();
        bool changed = false;
        for (int i = 0; i < array->getLength(); ++i) {
            const Object &child = array->getNF(i);
            if (!child.isRef()) {
                Object childCopy = child.copy();
                changed = rewriteDirectNamedDestinationReferences(&childCopy, oldName, newName) || changed;
            }
        }
        return changed;
    }
    if (object->isStream()) {
        Object streamDictionary(object->getStream()->getDict());
        return rewriteDirectNamedDestinationReferences(&streamDictionary, oldName, newName);
    }
    return false;
}

void rewriteNamedDestinationReferences(PDFDoc *document, const std::string &oldName, const std::string &newName)
{
    XRef *xref = document->getXRef();
    const int objectCount = xref->getNumObjects();
    for (int objectNumber = 0; objectNumber < objectCount; ++objectNumber) {
        const XRefEntry *entry = xref->getEntry(objectNumber);
        if (!entry || entry->type == xrefEntryFree || entry->type == xrefEntryNone) {
            continue;
        }
        const Ref reference { .num = objectNumber, .gen = entry->type == xrefEntryCompressed ? 0 : entry->gen };
        Object object = xref->fetch(reference);
        if (rewriteDirectNamedDestinationReferences(&object, oldName, newName)) {
            xref->setModifiedObject(&object, reference);
        }
    }
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

bool normalizedPointToUserCoordinates(Page *page, double normalizedX, double normalizedY, double *userX, double *userY)
{
    if (!page || !userX || !userY || normalizedX < 0.0 || normalizedX > 1.0 || normalizedY < 0.0 || normalizedY > 1.0) {
        return false;
    }

    // Match Poppler::LinkDestination's conversion so the destination lands at
    // the same normalized crop-box point that the viewer supplied.
    GfxState state(72.0, 72.0, page->getCropBox(), page->getRotate(), true);
    const std::array<double, 6> &ctm = state.getCTM();
    const double determinant = ctm[0] * ctm[3] - ctm[1] * ctm[2];
    if (std::abs(determinant) < 1e-12) {
        return false;
    }

    const double deviceX = normalizedX * page->getCropWidth();
    const double deviceY = normalizedY * page->getCropHeight();
    const double translatedX = deviceX - ctm[4];
    const double translatedY = deviceY - ctm[5];
    *userX = (ctm[3] * translatedX - ctm[2] * translatedY) / determinant;
    *userY = (-ctm[1] * translatedX + ctm[0] * translatedY) / determinant;
    return true;
}

Object makeXyzDestination(PDFDoc *doc, int pageNumber, double normalizedX, double normalizedY)
{
    Page *page = doc ? doc->getPage(pageNumber) : nullptr;
    Ref *pageRef = doc ? doc->getCatalog()->getPageRef(pageNumber) : nullptr;
    double userX = 0.0;
    double userY = 0.0;
    if (!page || !pageRef || !normalizedPointToUserCoordinates(page, normalizedX, normalizedY, &userX, &userY)) {
        return Object::null();
    }

    auto *destination = new Array(doc->getXRef());
    destination->add(Object(*pageRef));
    destination->add(Object(objName, "XYZ"));
    destination->add(Object(userX));
    destination->add(Object(userY));
    destination->add(Object::null());
    return Object(destination);
}

Object makeFitWidthDestination(PDFDoc *doc, int pageNumber, double normalizedY)
{
    Page *page = doc ? doc->getPage(pageNumber) : nullptr;
    Ref *pageRef = doc ? doc->getCatalog()->getPageRef(pageNumber) : nullptr;
    double unusedUserX = 0.0;
    double userY = 0.0;
    if (!page || !pageRef || !normalizedPointToUserCoordinates(page, 0.0, normalizedY, &unusedUserX, &userY)) {
        return Object::null();
    }

    auto *destination = new Array(doc->getXRef());
    destination->add(Object(*pageRef));
    destination->add(Object(objName, "FitH"));
    destination->add(Object(userY));
    return Object(destination);
}

struct NormalizedLinkRectangle
{
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
};

NormalizedLinkRectangle normalizedLinkRectangle(Page *page, const PDFRectangle &rectangle)
{
    double pageWidth = page->getCropWidth();
    double pageHeight = page->getCropHeight();
    if (page->getRotate() == 90 || page->getRotate() == 270) {
        std::swap(pageWidth, pageHeight);
    }

    GfxState state(72.0, 72.0, page->getCropBox(), page->getRotate(), true);
    double firstX = 0.0;
    double firstY = 0.0;
    double secondX = 0.0;
    double secondY = 0.0;
    state.transform(rectangle.x1, rectangle.y1, &firstX, &firstY);
    state.transform(rectangle.x2, rectangle.y2, &secondX, &secondY);

    // LinkExtractorOutputDev rounds to device pixels before normalizing.
    firstX = static_cast<int>(firstX + 0.5);
    firstY = static_cast<int>(firstY + 0.5);
    secondX = static_cast<int>(secondX + 0.5);
    secondY = static_cast<int>(secondY + 0.5);
    return { std::min(firstX, secondX) / pageWidth, std::min(firstY, secondY) / pageHeight, std::max(firstX, secondX) / pageWidth, std::max(firstY, secondY) / pageHeight };
}

double linkRectangleDistance(const NormalizedLinkRectangle &first, const NormalizedLinkRectangle &second)
{
    return std::abs(first.left - second.left) + std::abs(first.top - second.top) + std::abs(first.right - second.right) + std::abs(first.bottom - second.bottom);
}

PdfPageSequenceEditor::Result openEditablePdf(const std::string &inputFileName, const std::string &outputFileName, std::unique_ptr<PDFDoc> *document)
{
    if (!document || inputFileName.empty() || outputFileName.empty() || inputFileName == outputFileName) {
        return makeError(PdfPageSequenceEditor::Error::InvalidArguments, "Input and output files must be non-empty and different.");
    }

    ensureGlobalParams();
    auto doc = std::make_unique<PDFDoc>(std::make_unique<GooString>(inputFileName));
    if (!doc->isOk()) {
        return makeError(PdfPageSequenceEditor::Error::DamagedInput, "Could not edit damaged input PDF.");
    }
    if (doc->isEncrypted()) {
        return makeError(PdfPageSequenceEditor::Error::EncryptedInput, "Could not edit encrypted input PDF.", doc->getNumPages());
    }
    *document = std::move(doc);
    return { };
}

PdfPageSequenceEditor::Result saveEditedPdf(PDFDoc *document, const std::string &outputFileName)
{
    const int pageCount = document ? document->getNumPages() : 0;
    if (!document || document->saveAs(outputFileName, writeForceRewrite) != errNone) {
        return makeError(PdfPageSequenceEditor::Error::WriteError, "Could not write the edited PDF.", pageCount);
    }
    return { PdfPageSequenceEditor::Error::None, std::string(), pageCount, pageCount };
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

bool appendExistingPage(PDFDoc *doc, int pageNo, XRef *yRef, XRef *countRef, unsigned int numOffset, std::vector<PageEntry> *pages, const DestinationNameMap *clonedDestinations = nullptr)
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
        const DestinationNameMap clonedDestinations = ok ? cloneNamedDestinations(doc.get(), insertedDoc.get(), edit.insertPdfPage, *sourcePageRef, outputPageRef, edit.destinationConflictPolicy, &names, &dests) : DestinationNameMap { };
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

PdfPageSequenceEditor::Result combinePdfFilesImpl(const std::vector<std::string> &inputFileNames, const std::string &outputFileName, PdfPageSequenceEditor::NamedDestinationConflictPolicy conflictPolicy)
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

std::vector<Ref> currentPageRefs(PDFDoc *document)
{
    std::vector<Ref> refs;
    if (!document) {
        return refs;
    }
    const int pageCount = document->getNumPages();
    refs.reserve(pageCount);
    for (int pageNumber = 1; pageNumber <= pageCount; ++pageNumber) {
        Ref *pageRef = document->getCatalog()->getPageRef(pageNumber);
        if (!pageRef) {
            refs.clear();
            return refs;
        }
        refs.push_back(*pageRef);
    }
    return refs;
}

bool pageTreeIsFlat(PDFDoc *document, const std::vector<Ref> &pageRefs, Ref *rootPagesRef)
{
    XRef *xref = document->getXRef();
    Object catalog = xref->getCatalog();
    if (!catalog.isDict()) {
        return false;
    }
    const Object &rootRefObject = catalog.getDict()->lookupNF("Pages");
    if (!rootRefObject.isRef()) {
        return false;
    }
    *rootPagesRef = rootRefObject.getRef();
    Object root = xref->fetch(*rootPagesRef);
    Object kids = root.isDict() ? root.getDict()->lookupNF("Kids").copy() : Object::null();
    if (!kids.isArray() || kids.arrayGetLength() != static_cast<int>(pageRefs.size())) {
        return false;
    }
    for (int i = 0; i < kids.arrayGetLength(); ++i) {
        const Object &kid = kids.arrayGetNF(i);
        if (!kid.isRef() || kid.getRef() != pageRefs[static_cast<std::size_t>(i)]) {
            return false;
        }
    }
    return true;
}

bool makePagesIndependentOfInheritedAttributes(PDFDoc *document, const std::vector<Ref> &pageRefs)
{
    for (int pageNumber = 1; pageNumber <= static_cast<int>(pageRefs.size()); ++pageNumber) {
        Page *page = document->getPage(pageNumber);
        if (!page) {
            return false;
        }
        const PDFRectangle *cropBox = page->isCropped() ? page->getCropBox() : nullptr;
        if (!document->replacePageDict(pageNumber, page->getRotate(), page->getMediaBox(), cropBox)) {
            return false;
        }
        Object pageObject = document->getXRef()->fetch(pageRefs[static_cast<std::size_t>(pageNumber - 1)]);
        if (!pageObject.isDict()) {
            return false;
        }
        Object *resources = page->getResourceDictObject();
        if (resources && resources->isDict()) {
            pageObject.getDict()->set("Resources", resources->copy());
            document->getXRef()->setModifiedObject(&pageObject, pageRefs[static_cast<std::size_t>(pageNumber - 1)]);
        }
    }
    return true;
}

bool installFlatPageTree(PDFDoc *document, const std::vector<Ref> &pageRefs)
{
    if (!document || pageRefs.empty()) {
        return false;
    }

    const std::vector<Ref> existingPageRefs = currentPageRefs(document);
    if (existingPageRefs.empty()) {
        return false;
    }
    Ref rootPagesRef = Ref::INVALID();
    if (!pageTreeIsFlat(document, existingPageRefs, &rootPagesRef)) {
        if (rootPagesRef == Ref::INVALID() || !makePagesIndependentOfInheritedAttributes(document, existingPageRefs)) {
            return false;
        }
    }

    XRef *xref = document->getXRef();
    Object root = xref->fetch(rootPagesRef);
    if (!root.isDict()) {
        return false;
    }
    auto *kids = new Array(xref);
    for (const Ref &pageRef : pageRefs) {
        Object pageObject = xref->fetch(pageRef);
        if (!pageObject.isDict()) {
            return false;
        }
        pageObject.getDict()->set("Parent", Object(rootPagesRef));
        xref->setModifiedObject(&pageObject, pageRef);
        kids->add(Object(pageRef));
    }
    root.getDict()->set("Type", Object(objName, "Pages"));
    root.getDict()->set("Kids", Object(kids));
    root.getDict()->set("Count", Object(static_cast<int>(pageRefs.size())));
    xref->setModifiedObject(&root, rootPagesRef);
    document->invalidatePageTreeCache();
    return true;
}

bool readDecodedStream(Object *streamObject, std::vector<char> *data)
{
    if (!streamObject->streamRewind()) {
        return false;
    }
    unsigned char buffer[8192];
    while (true) {
        const int count = streamObject->streamGetChars(sizeof(buffer), buffer);
        if (count <= 0) {
            break;
        }
        data->insert(data->end(), reinterpret_cast<char *>(buffer), reinterpret_cast<char *>(buffer) + count);
    }
    streamObject->streamClose();
    return true;
}

bool isDecodedStreamKey(std::string_view key)
{
    return key != "Length" && key != "Filter" && key != "DecodeParms" && key != "F" && key != "FFilter" && key != "FDecodeParms" && key != "DL";
}

Object cloneObjectForPage(const Object &sourceObject,
                          XRef *sourceXref,
                          XRef *destinationXref,
                          const std::set<Ref> &sourcePageRefs,
                          bool preserveOtherPageRefs,
                          std::map<Ref, Ref> *copiedRefs,
                          int depth = 0)
{
    if (depth > 256) {
        return Object::null();
    }
    if (sourceObject.isRef()) {
        const Ref sourceRef = sourceObject.getRef();
        const auto existing = copiedRefs->find(sourceRef);
        if (existing != copiedRefs->end()) {
            return Object(existing->second);
        }
        if (sourcePageRefs.contains(sourceRef)) {
            return preserveOtherPageRefs ? Object(sourceRef) : Object::null();
        }
        Object fetched = sourceObject.fetch(sourceXref);
        if (fetched.isNull() || fetched.isError()) {
            return Object::null();
        }
        if (fetched.isStream()) {
            Object copy = cloneObjectForPage(fetched, sourceXref, destinationXref, sourcePageRefs, preserveOtherPageRefs, copiedRefs, depth + 1);
            if (!copy.isRef()) {
                return Object::null();
            }
            copiedRefs->emplace(sourceRef, copy.getRef());
            return copy;
        }
        const Ref destinationRef = destinationXref->addIndirectObject(Object::null());
        copiedRefs->emplace(sourceRef, destinationRef);
        Object copy = cloneObjectForPage(fetched, sourceXref, destinationXref, sourcePageRefs, preserveOtherPageRefs, copiedRefs, depth + 1);
        if (copy.isNull()) {
            return Object::null();
        }
        destinationXref->setModifiedObject(&copy, destinationRef);
        return Object(destinationRef);
    }
    if (sourceObject.isArray()) {
        auto *array = new Array(destinationXref);
        for (int i = 0; i < sourceObject.arrayGetLength(); ++i) {
            array->add(cloneObjectForPage(sourceObject.arrayGetNF(i), sourceXref, destinationXref, sourcePageRefs, preserveOtherPageRefs, copiedRefs, depth + 1));
        }
        return Object(array);
    }
    if (sourceObject.isDict()) {
        auto *dict = new Dict(destinationXref);
        for (int i = 0; i < sourceObject.dictGetLength(); ++i) {
            dict->set(sourceObject.dictGetKey(i), cloneObjectForPage(sourceObject.dictGetValNF(i), sourceXref, destinationXref, sourcePageRefs, preserveOtherPageRefs, copiedRefs, depth + 1));
        }
        return Object(dict);
    }
    if (sourceObject.isStream()) {
        Object streamObject = sourceObject.copy();
        std::vector<char> data;
        if (!readDecodedStream(&streamObject, &data)) {
            return Object::null();
        }
        auto *dict = new Dict(destinationXref);
        Dict *sourceDict = streamObject.streamGetDict();
        for (int i = 0; i < sourceDict->getLength(); ++i) {
            const char *key = sourceDict->getKey(i);
            if (isDecodedStreamKey(key)) {
                dict->set(key, cloneObjectForPage(sourceDict->getValNF(i), sourceXref, destinationXref, sourcePageRefs, preserveOtherPageRefs, copiedRefs, depth + 1));
            }
        }
        return Object(destinationXref->addStreamObject(dict, std::move(data), StreamCompression::None));
    }
    return sourceObject.copy();
}

void persistCatalogChild(PDFDoc *document, Object *catalog, const char *key, Object object, const Ref &reference)
{
    XRef *xref = document->getXRef();
    if (reference != Ref::INVALID()) {
        xref->setModifiedObject(&object, reference);
    } else {
        catalog->getDict()->set(key, std::move(object));
        xref->setModifiedObject(catalog, xref->getRoot());
    }
}

void removeStateDestinations(PDFDoc *document, const PdfPageSequenceEditor::LivePageStatePtr &state)
{
    if (!state || state->namedDestinations.empty()) {
        return;
    }
    std::set<std::string> namesToRemove;
    for (const auto &[name, destination] : state->namedDestinations) {
        namesToRemove.insert(name);
    }
    XRef *xref = document->getXRef();
    Object catalog = xref->getCatalog();
    std::vector<NamedDestinationEntry> entries = materializeNameTreeDestinations(document);
    std::erase_if(entries, [&](const NamedDestinationEntry &entry) { return namesToRemove.contains(entry.name); });
    Ref namesRef = Ref::INVALID();
    Object names = catalog.getDict()->lookup("Names", &namesRef);
    installDestinationNameTree(document, &names, std::move(entries));
    persistCatalogChild(document, &catalog, "Names", std::move(names), namesRef);

    Ref legacyRef = Ref::INVALID();
    Object legacy = catalog.getDict()->lookup("Dests", &legacyRef);
    if (legacy.isDict()) {
        for (const std::string &name : namesToRemove) {
            legacy.getDict()->remove(name);
        }
        persistCatalogChild(document, &catalog, "Dests", std::move(legacy), legacyRef);
    }
    document->getCatalog()->invalidateNamedDestinationCache();
}

void restoreStateDestinations(PDFDoc *document, const PdfPageSequenceEditor::LivePageStatePtr &state)
{
    if (!state || state->namedDestinations.empty()) {
        return;
    }
    XRef *xref = document->getXRef();
    Object catalog = xref->getCatalog();
    std::vector<NamedDestinationEntry> entries = materializeNameTreeDestinations(document);
    for (const auto &[name, destination] : state->namedDestinations) {
        std::erase_if(entries, [&](const NamedDestinationEntry &entry) { return entry.name == name; });
        entries.push_back(NamedDestinationEntry { name, destination.copy() });
    }
    Ref namesRef = Ref::INVALID();
    Object names = catalog.getDict()->lookup("Names", &namesRef);
    installDestinationNameTree(document, &names, std::move(entries));
    persistCatalogChild(document, &catalog, "Names", std::move(names), namesRef);

    Ref legacyRef = Ref::INVALID();
    Object legacy = catalog.getDict()->lookup("Dests", &legacyRef);
    if (legacy.isDict()) {
        for (const auto &[name, destination] : state->namedDestinations) {
            legacy.getDict()->set(name, destination.copy());
        }
        persistCatalogChild(document, &catalog, "Dests", std::move(legacy), legacyRef);
    }
    document->getCatalog()->invalidateNamedDestinationCache();
}

bool rewriteClonedPageLinks(PDFDoc *document, const Ref &pageRef, const DestinationNameMap &destinations)
{
    Object page = document->getXRef()->fetch(pageRef);
    Object annotations = page.isDict() ? page.getDict()->lookup("Annots") : Object::null();
    if (!annotations.isArray()) {
        return true;
    }
    for (int i = 0; i < annotations.arrayGetLength(); ++i) {
        const Object &annotationRef = annotations.arrayGetNF(i);
        Object annotation = annotations.arrayGet(i);
        if (!annotation.isDict()) {
            continue;
        }
        if (!destinations.empty()) {
            rewriteClonedNamedLinks(document, annotation.getDict(), destinations);
        }
        annotation.getDict()->remove("NM");
        annotation.getDict()->set("P", Object(pageRef));
        if (annotationRef.isRef()) {
            document->getXRef()->setModifiedObject(&annotation, annotationRef.getRef());
        }
    }
    return true;
}

}

namespace PdfPageSequenceEditor {

Result insertBlankPageAfter(PDFDoc *document, int pageNumber, double width, double height, LivePageStatePtr *state)
{
    const int pageCount = document ? document->getNumPages() : 0;
    if (!document || !state || pageNumber < 0 || pageNumber > pageCount || width <= 0.0 || height <= 0.0) {
        return makeError(Error::InvalidArguments, "The blank-page insertion arguments are invalid.", pageCount);
    }

    std::vector<Ref> pageRefs = currentPageRefs(document);
    if (static_cast<int>(pageRefs.size()) != pageCount) {
        return makeError(Error::PageError, "Could not read the current page tree.", pageCount);
    }
    XRef *xref = document->getXRef();
    const PDFRectangle mediaBox(0.0, 0.0, width, height);
    Object page = makeBlankPage(xref, &mediaBox, nullptr, 0);
    const Ref pageRef = xref->addIndirectObject(page);
    pageRefs.insert(pageRefs.begin() + pageNumber, pageRef);
    if (!installFlatPageTree(document, pageRefs)) {
        return makeError(Error::PageError, "Could not update the page tree.", pageCount);
    }
    auto resultState = std::make_shared<LivePageState>();
    resultState->pageRef = pageRef;
    resultState->attached = true;
    *state = std::move(resultState);
    return { Error::None, std::string(), pageCount, pageCount + 1 };
}

Result duplicatePageAfter(PDFDoc *document, int sourcePageNumber, int pageNumber, NamedDestinationConflictPolicy conflictPolicy, LivePageStatePtr *state)
{
    const int pageCount = document ? document->getNumPages() : 0;
    if (!document || !state || sourcePageNumber < 1 || sourcePageNumber > pageCount || pageNumber < 0 || pageNumber > pageCount) {
        return makeError(Error::InvalidArguments, "The page duplication arguments are invalid.", pageCount);
    }
    std::vector<Ref> pageRefs = currentPageRefs(document);
    if (static_cast<int>(pageRefs.size()) != pageCount) {
        return makeError(Error::PageError, "Could not read the current page tree.", pageCount);
    }
    Page *sourcePage = document->getPage(sourcePageNumber);
    const Ref sourcePageRef = pageRefs[static_cast<std::size_t>(sourcePageNumber - 1)];
    const PDFRectangle *cropBox = sourcePage && sourcePage->isCropped() ? sourcePage->getCropBox() : nullptr;
    if (!sourcePage || !document->replacePageDict(sourcePageNumber, sourcePage->getRotate(), sourcePage->getMediaBox(), cropBox)) {
        return makeError(Error::PageError, "Could not prepare the page to duplicate.", pageCount);
    }
    Object sourcePageObject = document->getXRef()->fetch(sourcePageRef);
    Object *resources = sourcePage->getResourceDictObject();
    if (resources && resources->isDict()) {
        sourcePageObject.getDict()->set("Resources", resources->copy());
        document->getXRef()->setModifiedObject(&sourcePageObject, sourcePageRef);
    }

    XRef *xref = document->getXRef();
    const Ref destinationPageRef = xref->addIndirectObject(Object::null());
    auto resultState = std::make_shared<LivePageState>();
    resultState->pageRef = destinationPageRef;

    Object catalog = xref->getCatalog();
    Ref namesRef = Ref::INVALID();
    Ref legacyRef = Ref::INVALID();
    Object names = catalog.getDict()->lookup("Names", &namesRef);
    Object legacy = catalog.getDict()->lookup("Dests", &legacyRef);
    const DestinationNameMap clonedDestinations = cloneNamedDestinations(document,
                                                                         document,
                                                                         sourcePageNumber,
                                                                         sourcePageRef,
                                                                         destinationPageRef,
                                                                         conflictPolicy,
                                                                         &names,
                                                                         &legacy,
                                                                         &resultState->namedDestinations);
    if (!resultState->namedDestinations.empty()) {
        persistCatalogChild(document, &catalog, "Names", std::move(names), namesRef);
        if (legacy.isDict()) {
            persistCatalogChild(document, &catalog, "Dests", std::move(legacy), legacyRef);
        }
        document->getCatalog()->invalidateNamedDestinationCache();
    }

    const std::set<Ref> sourcePageRefs(pageRefs.begin(), pageRefs.end());
    std::map<Ref, Ref> copiedRefs;
    copiedRefs.emplace(sourcePageRef, destinationPageRef);
    auto *pageDict = new Dict(xref);
    for (int i = 0; i < sourcePageObject.dictGetLength(); ++i) {
        const char *key = sourcePageObject.dictGetKey(i);
        if (std::strcmp(key, "Parent") == 0) {
            continue;
        }
        if (std::strcmp(key, "Annots") == 0) {
            pageDict->set(key, cloneObjectForPage(sourcePageObject.dictGetValNF(i), xref, xref, sourcePageRefs, true, &copiedRefs));
        } else {
            pageDict->set(key, sourcePageObject.dictGetValNF(i).deepCopy());
        }
    }
    Object copiedPage(pageDict);
    xref->setModifiedObject(&copiedPage, destinationPageRef);
    rewriteClonedPageLinks(document, destinationPageRef, clonedDestinations);

    pageRefs.insert(pageRefs.begin() + pageNumber, destinationPageRef);
    if (!installFlatPageTree(document, pageRefs)) {
        removeStateDestinations(document, resultState);
        return makeError(Error::PageError, "Could not update the page tree.", pageCount);
    }
    resultState->attached = true;
    *state = std::move(resultState);
    return { Error::None, std::string(), pageCount, pageCount + 1 };
}

Result insertPdfPageAfter(PDFDoc *document,
                          int pageNumber,
                          const std::string &insertedFileName,
                          int pageToInsert,
                          NamedDestinationConflictPolicy conflictPolicy,
                          LivePageStatePtr *state)
{
    const int pageCount = document ? document->getNumPages() : 0;
    if (!document || !state || pageNumber < 0 || pageNumber > pageCount || insertedFileName.empty() || pageToInsert < 1) {
        return makeError(Error::InvalidArguments, "The imported-page insertion arguments are invalid.", pageCount);
    }
    ensureGlobalParams();
    auto sourceDocument = std::make_unique<PDFDoc>(std::make_unique<GooString>(insertedFileName));
    if (!sourceDocument->isOk() || !sourceDocument->getXRef()->getCatalog().isDict()) {
        return makeError(Error::DamagedInput, "Could not read the imported PDF page.", pageCount);
    }
    if (sourceDocument->isEncrypted()) {
        return makeError(Error::EncryptedInput, "Could not import a page from an encrypted PDF.", pageCount);
    }
    if (pageToInsert > sourceDocument->getNumPages()) {
        return makeError(Error::InvalidArguments, "The imported page is outside the source PDF page range.", pageCount);
    }

    std::vector<Ref> pageRefs = currentPageRefs(document);
    if (static_cast<int>(pageRefs.size()) != pageCount) {
        return makeError(Error::PageError, "Could not read the current page tree.", pageCount);
    }
    Page *sourcePage = sourceDocument->getPage(pageToInsert);
    Ref *sourcePageRef = sourceDocument->getCatalog()->getPageRef(pageToInsert);
    if (!sourcePage || !sourcePageRef) {
        return makeError(Error::PageError, "Could not read the imported PDF page.", pageCount);
    }
    const PDFRectangle *cropBox = sourcePage->isCropped() ? sourcePage->getCropBox() : nullptr;
    if (!sourceDocument->replacePageDict(pageToInsert, sourcePage->getRotate(), sourcePage->getMediaBox(), cropBox)) {
        return makeError(Error::PageError, "Could not prepare the imported PDF page.", pageCount);
    }
    Object sourcePageObject = sourceDocument->getXRef()->fetch(*sourcePageRef);
    Object *resources = sourcePage->getResourceDictObject();
    if (resources && resources->isDict()) {
        sourcePageObject.getDict()->set("Resources", resources->copy());
    }

    XRef *destinationXref = document->getXRef();
    const Ref destinationPageRef = destinationXref->addIndirectObject(Object::null());
    std::map<Ref, Ref> copiedRefs;
    copiedRefs.emplace(*sourcePageRef, destinationPageRef);
    std::set<Ref> sourcePageRefs;
    for (int sourcePageNumber = 1; sourcePageNumber <= sourceDocument->getNumPages(); ++sourcePageNumber) {
        if (Ref *ref = sourceDocument->getCatalog()->getPageRef(sourcePageNumber)) {
            sourcePageRefs.insert(*ref);
        }
    }
    auto resultState = std::make_shared<LivePageState>();
    resultState->pageRef = destinationPageRef;

    Object catalog = destinationXref->getCatalog();
    Ref namesRef = Ref::INVALID();
    Ref legacyRef = Ref::INVALID();
    Object names = catalog.getDict()->lookup("Names", &namesRef);
    Object legacy = catalog.getDict()->lookup("Dests", &legacyRef);
    const DestinationNameMap clonedDestinations = cloneNamedDestinations(document,
                                                                         sourceDocument.get(),
                                                                         pageToInsert,
                                                                         *sourcePageRef,
                                                                         destinationPageRef,
                                                                         conflictPolicy,
                                                                         &names,
                                                                         &legacy,
                                                                         &resultState->namedDestinations);
    if (!resultState->namedDestinations.empty()) {
        persistCatalogChild(document, &catalog, "Names", std::move(names), namesRef);
        if (legacy.isDict()) {
            persistCatalogChild(document, &catalog, "Dests", std::move(legacy), legacyRef);
        }
        document->getCatalog()->invalidateNamedDestinationCache();
    }

    auto *pageDict = new Dict(destinationXref);
    for (int i = 0; i < sourcePageObject.dictGetLength(); ++i) {
        const char *key = sourcePageObject.dictGetKey(i);
        if (std::strcmp(key, "Parent") == 0) {
            continue;
        }
        pageDict->set(key,
                      cloneObjectForPage(sourcePageObject.dictGetValNF(i),
                                         sourceDocument->getXRef(),
                                         destinationXref,
                                         sourcePageRefs,
                                         false,
                                         &copiedRefs));
    }
    Object copiedPage(pageDict);
    destinationXref->setModifiedObject(&copiedPage, destinationPageRef);
    rewriteClonedPageLinks(document, destinationPageRef, clonedDestinations);

    pageRefs.insert(pageRefs.begin() + pageNumber, destinationPageRef);
    if (!installFlatPageTree(document, pageRefs)) {
        removeStateDestinations(document, resultState);
        return makeError(Error::PageError, "Could not update the page tree.", pageCount);
    }
    resultState->attached = true;
    *state = std::move(resultState);
    return { Error::None, std::string(), pageCount, pageCount + 1 };
}

Result detachPage(PDFDoc *document, int pageNumber, LivePageStatePtr *state)
{
    const int pageCount = document ? document->getNumPages() : 0;
    if (!document || !state || pageNumber < 1 || pageNumber > pageCount || pageCount <= 1) {
        return makeError(Error::InvalidArguments, "The page deletion arguments are invalid.", pageCount);
    }
    std::vector<Ref> refs = currentPageRefs(document);
    if (static_cast<int>(refs.size()) != pageCount) {
        return makeError(Error::PageError, "Could not read the current page tree.", pageCount);
    }
    auto resultState = std::make_shared<LivePageState>();
    resultState->pageRef = refs[static_cast<std::size_t>(pageNumber - 1)];
    resultState->attached = true;
    Result result = detachPage(document, pageNumber, resultState);
    if (result.ok()) {
        *state = std::move(resultState);
    }
    return result;
}

Result detachPage(PDFDoc *document, int pageNumber, const LivePageStatePtr &state)
{
    const int pageCount = document ? document->getNumPages() : 0;
    if (!document || !state || !state->attached || pageNumber < 1 || pageNumber > pageCount || pageCount <= 1) {
        return makeError(Error::InvalidArguments, "The page deletion arguments are invalid.", pageCount);
    }
    std::vector<Ref> refs = currentPageRefs(document);
    if (static_cast<int>(refs.size()) != pageCount || refs[static_cast<std::size_t>(pageNumber - 1)] != state->pageRef) {
        return makeError(Error::PageError, "The page edit state no longer matches the document.", pageCount);
    }
    refs.erase(refs.begin() + (pageNumber - 1));
    if (!installFlatPageTree(document, refs)) {
        return makeError(Error::PageError, "Could not update the page tree.", pageCount);
    }
    removeStateDestinations(document, state);
    state->attached = false;
    return { Error::None, std::string(), pageCount, pageCount - 1 };
}

Result attachPageAfter(PDFDoc *document, int pageNumber, const LivePageStatePtr &state)
{
    const int pageCount = document ? document->getNumPages() : 0;
    if (!document || !state || state->attached || state->pageRef == Ref::INVALID() || pageNumber < 0 || pageNumber > pageCount) {
        return makeError(Error::InvalidArguments, "The page insertion state is invalid.", pageCount);
    }
    std::vector<Ref> refs = currentPageRefs(document);
    if (static_cast<int>(refs.size()) != pageCount) {
        return makeError(Error::PageError, "Could not read the current page tree.", pageCount);
    }
    refs.insert(refs.begin() + pageNumber, state->pageRef);
    if (!installFlatPageTree(document, refs)) {
        return makeError(Error::PageError, "Could not update the page tree.", pageCount);
    }
    restoreStateDestinations(document, state);
    state->attached = true;
    return { Error::None, std::string(), pageCount, pageCount + 1 };
}

Result movePage(PDFDoc *document, int sourcePageNumber, int destinationPageNumber)
{
    const int pageCount = document ? document->getNumPages() : 0;
    if (!document || sourcePageNumber < 1 || sourcePageNumber > pageCount || destinationPageNumber < 1 || destinationPageNumber > pageCount) {
        return makeError(Error::InvalidArguments, "The page move is outside the document page range.", pageCount);
    }
    std::vector<Ref> refs = currentPageRefs(document);
    if (static_cast<int>(refs.size()) != pageCount) {
        return makeError(Error::PageError, "Could not read the current page tree.", pageCount);
    }
    const Ref moved = refs[static_cast<std::size_t>(sourcePageNumber - 1)];
    refs.erase(refs.begin() + (sourcePageNumber - 1));
    refs.insert(refs.begin() + (destinationPageNumber - 1), moved);
    if (!installFlatPageTree(document, refs)) {
        return makeError(Error::PageError, "Could not update the page tree.", pageCount);
    }
    return { Error::None, std::string(), pageCount, pageCount };
}

Result rotatePage(PDFDoc *document, int pageNumber, int rotationDegrees)
{
    const int pageCount = document ? document->getNumPages() : 0;
    if (!document || pageNumber < 1 || pageNumber > pageCount || rotationDegrees < 0 || rotationDegrees >= 360 || rotationDegrees % 90 != 0) {
        return makeError(Error::InvalidArguments, "The page rotation arguments are invalid.", pageCount);
    }
    Page *page = document->getPage(pageNumber);
    Ref *pageRef = document->getCatalog()->getPageRef(pageNumber);
    if (!page || !pageRef) {
        return makeError(Error::PageError, "Could not read the page to rotate.", pageCount);
    }
    Object pageObject = document->getXRef()->fetch(*pageRef);
    if (!pageObject.isDict()) {
        return makeError(Error::PageError, "Could not read the page dictionary.", pageCount);
    }
    pageObject.getDict()->set("Rotate", Object(rotationDegrees));
    document->getXRef()->setModifiedObject(&pageObject, *pageRef);
    document->invalidatePageTreeCache();
    return { Error::None, std::string(), pageCount, pageCount };
}

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

Result insertPdfPageAfter(const std::string &inputFileName, const std::string &outputFileName, int pageNumber, const std::string &insertedFileName, int pageToInsert, NamedDestinationConflictPolicy conflictPolicy)
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

Result setNamedDestination(PDFDoc *document, const std::string &name, int pageNumber, double normalizedX, double normalizedY, NamedDestinationView view)
{
    const int pageCount = document ? document->getNumPages() : 0;
    if (name.empty() || pageNumber < 1 || pageNumber > pageCount) {
        return makeError(Error::InvalidArguments, "The named destination name or page is invalid.", pageCount);
    }

    Object destination = view == NamedDestinationView::FitWidth ? makeFitWidthDestination(document, pageNumber, normalizedY) : makeXyzDestination(document, pageNumber, normalizedX, normalizedY);
    if (!destination.isArray()) {
        return makeError(Error::InvalidArguments, "The named destination position is invalid.", pageCount);
    }

    XRef *xref = document->getXRef();
    Object catalog = xref->getCatalog();
    if (!catalog.isDict()) {
        return makeError(Error::DamagedInput, "The PDF catalog is invalid.", pageCount);
    }

    std::vector<NamedDestinationEntry> entries = materializeNameTreeDestinations(document);
    std::erase_if(entries, [&](const NamedDestinationEntry &entry) { return entry.name == name; });
    entries.push_back(NamedDestinationEntry { name, destination.copy() });

    Ref namesRef = Ref::INVALID();
    Object names = catalog.getDict()->lookup("Names", &namesRef);
    installDestinationNameTree(document, &names, std::move(entries));
    if (namesRef != Ref::INVALID()) {
        xref->setModifiedObject(&names, namesRef);
    } else {
        catalog.getDict()->set("Names", std::move(names));
        xref->setModifiedObject(&catalog, { .num = xref->getRootNum(), .gen = xref->getRootGen() });
    }

    // A legacy /Dests dictionary takes precedence over the name tree in
    // Poppler. Mirror the value there when necessary so the target resolves.
    Ref legacyDestsRef = Ref::INVALID();
    Object legacyDests = catalog.getDict()->lookup("Dests", &legacyDestsRef);
    if (legacyDests.isDict()) {
        legacyDests.getDict()->set(name, destination.copy());
        if (legacyDestsRef != Ref::INVALID()) {
            xref->setModifiedObject(&legacyDests, legacyDestsRef);
        } else {
            xref->setModifiedObject(&catalog, { .num = xref->getRootNum(), .gen = xref->getRootGen() });
        }
    }

    document->getCatalog()->invalidateNamedDestinationCache();

    return { Error::None, std::string(), pageCount, pageCount };
}

Result renameNamedDestination(PDFDoc *document, const std::string &oldName, const std::string &newName)
{
    const int pageCount = document ? document->getNumPages() : 0;
    if (!document || oldName.empty() || newName.empty()) {
        return makeError(Error::InvalidArguments, "The named destination name is invalid.", pageCount);
    }
    if (oldName == newName) {
        return { Error::None, std::string(), pageCount, pageCount };
    }

    const GooString oldNameString(oldName);
    const std::unique_ptr<LinkDest> oldDestination = document->getCatalog()->findDest(&oldNameString);
    if (!oldDestination || !oldDestination->isOk()) {
        return makeError(Error::InvalidArguments, "The named destination to rename does not exist.", pageCount);
    }
    const std::optional<Ref> pageRef = destinationPageRef(document, *oldDestination);
    if (!pageRef) {
        return makeError(Error::PageError, "The named destination page could not be found.", pageCount);
    }
    Object destination = makeExplicitDestination(document->getXRef(), *oldDestination, *pageRef);

    XRef *xref = document->getXRef();
    Object catalog = xref->getCatalog();
    if (!catalog.isDict()) {
        return makeError(Error::DamagedInput, "The PDF catalog is invalid.", pageCount);
    }

    std::vector<NamedDestinationEntry> entries = materializeNameTreeDestinations(document);
    std::erase_if(entries, [&](const NamedDestinationEntry &entry) { return entry.name == oldName || entry.name == newName; });
    entries.push_back(NamedDestinationEntry { newName, destination.copy() });

    Ref namesRef = Ref::INVALID();
    Object names = catalog.getDict()->lookup("Names", &namesRef);
    installDestinationNameTree(document, &names, std::move(entries));
    if (namesRef != Ref::INVALID()) {
        xref->setModifiedObject(&names, namesRef);
    } else {
        catalog.getDict()->set("Names", std::move(names));
        xref->setModifiedObject(&catalog, xref->getRoot());
    }

    Ref legacyDestsRef = Ref::INVALID();
    Object legacyDests = catalog.getDict()->lookup("Dests", &legacyDestsRef);
    if (legacyDests.isDict()) {
        legacyDests.getDict()->remove(oldName);
        legacyDests.getDict()->remove(newName);
        legacyDests.getDict()->set(newName, destination.copy());
        if (legacyDestsRef != Ref::INVALID()) {
            xref->setModifiedObject(&legacyDests, legacyDestsRef);
        } else {
            xref->setModifiedObject(&catalog, xref->getRoot());
        }
    }

    rewriteNamedDestinationReferences(document, oldName, newName);
    document->getCatalog()->invalidateNamedDestinationCache();
    return { Error::None, std::string(), pageCount, pageCount };
}

Result deleteNamedDestination(PDFDoc *document, const std::string &name)
{
    const int pageCount = document ? document->getNumPages() : 0;
    if (!document || name.empty()) {
        return makeError(Error::InvalidArguments, "The named destination name is invalid.", pageCount);
    }

    const GooString nameString(name);
    const std::unique_ptr<LinkDest> destination = document->getCatalog()->findDest(&nameString);
    if (!destination || !destination->isOk()) {
        return makeError(Error::InvalidArguments, "The named destination to delete does not exist.", pageCount);
    }

    XRef *xref = document->getXRef();
    Object catalog = xref->getCatalog();
    if (!catalog.isDict()) {
        return makeError(Error::DamagedInput, "The PDF catalog is invalid.", pageCount);
    }

    std::vector<NamedDestinationEntry> entries = materializeNameTreeDestinations(document);
    std::erase_if(entries, [&](const NamedDestinationEntry &entry) { return entry.name == name; });

    Ref namesRef = Ref::INVALID();
    Object names = catalog.getDict()->lookup("Names", &namesRef);
    installDestinationNameTree(document, &names, std::move(entries));
    if (namesRef != Ref::INVALID()) {
        xref->setModifiedObject(&names, namesRef);
    } else {
        catalog.getDict()->set("Names", std::move(names));
        xref->setModifiedObject(&catalog, xref->getRoot());
    }

    Ref legacyDestsRef = Ref::INVALID();
    Object legacyDests = catalog.getDict()->lookup("Dests", &legacyDestsRef);
    if (legacyDests.isDict()) {
        legacyDests.getDict()->remove(name);
        if (legacyDestsRef != Ref::INVALID()) {
            xref->setModifiedObject(&legacyDests, legacyDestsRef);
        } else {
            xref->setModifiedObject(&catalog, xref->getRoot());
        }
    }

    document->getCatalog()->invalidateNamedDestinationCache();
    return { Error::None, std::string(), pageCount, pageCount };
}

static Result editLinkDestination(PDFDoc *document,
                                  int sourcePageNumber,
                                  double linkLeft,
                                  double linkTop,
                                  double linkRight,
                                  double linkBottom,
                                  const std::string &destinationName,
                                  int destinationPageNumber,
                                  double destinationX,
                                  double destinationY,
                                  const std::string &externalUrl,
                                  bool useExternalUrl)
{
    if (!document) {
        return makeError(Error::InvalidArguments, "The PDF document is not open.");
    }
    const int pageCount = document->getNumPages();
    if (sourcePageNumber < 1 || sourcePageNumber > pageCount || linkLeft < 0.0 || linkTop < 0.0 || linkRight > 1.0 || linkBottom > 1.0 || linkLeft > linkRight || linkTop > linkBottom) {
        return makeError(Error::InvalidArguments, "The source link position is invalid.", pageCount);
    }
    if (useExternalUrl && externalUrl.empty()) {
        return makeError(Error::InvalidArguments, "The external link URL is invalid.", pageCount);
    }
    if (!useExternalUrl && destinationName.empty() && (destinationPageNumber < 1 || destinationPageNumber > pageCount)) {
        return makeError(Error::InvalidArguments, "The destination page is invalid.", pageCount);
    }

    Page *page = document->getPage(sourcePageNumber);
    if (!page) {
        return makeError(Error::PageError, "Could not read the source link page.", pageCount);
    }

    const NormalizedLinkRectangle requested { linkLeft, linkTop, linkRight, linkBottom };
    std::shared_ptr<AnnotLink> selectedLink;
    double selectedDistance = std::numeric_limits<double>::max();
    for (const std::shared_ptr<Annot> &annotation : page->getAnnots()->getAnnots()) {
        if (!annotation || annotation->getType() != Annot::typeLink) {
            continue;
        }
        auto link = std::static_pointer_cast<AnnotLink>(annotation);
        const double distance = linkRectangleDistance(requested, normalizedLinkRectangle(page, link->getRect()));
        if (distance < selectedDistance) {
            selectedDistance = distance;
            selectedLink = std::move(link);
        }
    }
    if (!selectedLink || selectedDistance > 0.04) {
        return makeError(Error::PageError, "Could not find the selected link.", pageCount);
    }

    Object destination;
    if (!useExternalUrl && !destinationName.empty()) {
        const GooString destinationNameString(destinationName);
        const std::unique_ptr<LinkDest> resolved = document->getCatalog()->findDest(&destinationNameString);
        if (!resolved || !resolved->isOk()) {
            return makeError(Error::InvalidArguments, "The selected named destination does not exist.", pageCount);
        }
        destination = Object(std::string(destinationName));
    } else if (!useExternalUrl) {
        destination = makeXyzDestination(document, destinationPageNumber, destinationX, destinationY);
        if (!destination.isArray()) {
            return makeError(Error::InvalidArguments, "The direct destination position is invalid.", pageCount);
        }
    }

    const Object &annotationObject = selectedLink->getAnnotObj();
    if (!annotationObject.isDict()) {
        return makeError(Error::PageError, "The selected link annotation is invalid.", pageCount);
    }
    XRef *xref = document->getXRef();
    if (useExternalUrl) {
        auto *action = new Dict(xref);
        action->set("S", Object(objName, "URI"));
        action->set("URI", Object(std::string(externalUrl)));
        annotationObject.getDict()->remove("Dest");
        annotationObject.getDict()->set("A", Object(action));
    } else {
        annotationObject.getDict()->remove("A");
        annotationObject.getDict()->set("Dest", std::move(destination));
    }
    selectedLink->reloadAction();

    if (selectedLink->getHasRef()) {
        xref->setModifiedObject(&annotationObject, selectedLink->getRef());
    } else {
        const Object &annotsReference = page->getPageObj().dictLookupNF("Annots");
        if (annotsReference.isRef()) {
            Object annots = annotsReference.fetch(xref);
            xref->setModifiedObject(&annots, annotsReference.getRef());
        } else {
            xref->setModifiedObject(&page->getPageObj(), page->getRef());
        }
    }

    return { Error::None, std::string(), pageCount, pageCount };
}

Result editInternalLinkDestination(PDFDoc *document, int sourcePageNumber, double linkLeft, double linkTop, double linkRight, double linkBottom, const std::string &destinationName, int destinationPageNumber, double destinationX,
                                   double destinationY)
{
    return editLinkDestination(document, sourcePageNumber, linkLeft, linkTop, linkRight, linkBottom, destinationName, destinationPageNumber, destinationX, destinationY, std::string(), false);
}

Result editExternalLinkDestination(PDFDoc *document, int sourcePageNumber, double linkLeft, double linkTop, double linkRight, double linkBottom, const std::string &url)
{
    return editLinkDestination(document, sourcePageNumber, linkLeft, linkTop, linkRight, linkBottom, std::string(), 0, 0.0, 0.0, url, true);
}

static Result createLink(PDFDoc *document,
                         int sourcePageNumber,
                         double linkLeft,
                         double linkTop,
                         double linkRight,
                         double linkBottom,
                         const std::string &destinationName,
                         int destinationPageNumber,
                         double destinationX,
                         double destinationY,
                         const std::string &externalUrl,
                         bool useExternalUrl)
{
    if (!document) {
        return makeError(Error::InvalidArguments, "The PDF document is not open.");
    }
    const int pageCount = document->getNumPages();
    if (sourcePageNumber < 1 || sourcePageNumber > pageCount || linkLeft < 0.0 || linkTop < 0.0 || linkRight > 1.0 || linkBottom > 1.0 || linkLeft >= linkRight || linkTop >= linkBottom) {
        return makeError(Error::InvalidArguments, "The source link position is invalid.", pageCount);
    }
    if (useExternalUrl && externalUrl.empty()) {
        return makeError(Error::InvalidArguments, "The external link URL is invalid.", pageCount);
    }
    if (!useExternalUrl && destinationName.empty() && (destinationPageNumber < 1 || destinationPageNumber > pageCount)) {
        return makeError(Error::InvalidArguments, "The destination page is invalid.", pageCount);
    }

    Page *page = document->getPage(sourcePageNumber);
    if (!page) {
        return makeError(Error::PageError, "Could not read the source link page.", pageCount);
    }

    Object destination;
    if (!useExternalUrl && !destinationName.empty()) {
        const GooString destinationNameString(destinationName);
        const std::unique_ptr<LinkDest> resolved = document->getCatalog()->findDest(&destinationNameString);
        if (!resolved || !resolved->isOk()) {
            return makeError(Error::InvalidArguments, "The selected named destination does not exist.", pageCount);
        }
        destination = Object(std::string(destinationName));
    } else if (!useExternalUrl) {
        destination = makeXyzDestination(document, destinationPageNumber, destinationX, destinationY);
        if (!destination.isArray()) {
            return makeError(Error::InvalidArguments, "The direct destination position is invalid.", pageCount);
        }
    }

    double firstX = 0.0;
    double firstY = 0.0;
    double secondX = 0.0;
    double secondY = 0.0;
    if (!normalizedPointToUserCoordinates(page, linkLeft, linkTop, &firstX, &firstY) || !normalizedPointToUserCoordinates(page, linkRight, linkBottom, &secondX, &secondY)) {
        return makeError(Error::InvalidArguments, "The source link position is invalid.", pageCount);
    }

    PDFRectangle rectangle(std::min(firstX, secondX), std::min(firstY, secondY), std::max(firstX, secondX), std::max(firstY, secondY));
    auto link = std::make_shared<AnnotLink>(document, &rectangle);
    const Object &annotationObject = link->getAnnotObj();
    if (!link->isOk() || !annotationObject.isDict()) {
        return makeError(Error::PageError, "Could not create the link annotation.", pageCount);
    }
    if (useExternalUrl) {
        auto *action = new Dict(document->getXRef());
        action->set("S", Object(objName, "URI"));
        action->set("URI", Object(std::string(externalUrl)));
        annotationObject.getDict()->set("A", Object(action));
    } else {
        annotationObject.getDict()->set("Dest", std::move(destination));
    }
    link->reloadAction();
    auto *border = new Array(document->getXRef());
    border->add(Object(0));
    border->add(Object(0));
    border->add(Object(0));
    annotationObject.getDict()->set("Border", Object(border));
    if (!page->addAnnot(link)) {
        return makeError(Error::PageError, "Could not add the link annotation to the page.", pageCount);
    }
    document->getXRef()->setModifiedObject(&annotationObject, link->getRef());

    return { Error::None, std::string(), pageCount, pageCount };
}

Result createInternalLink(PDFDoc *document, int sourcePageNumber, double linkLeft, double linkTop, double linkRight, double linkBottom, const std::string &destinationName, int destinationPageNumber, double destinationX, double destinationY)
{
    return createLink(document, sourcePageNumber, linkLeft, linkTop, linkRight, linkBottom, destinationName, destinationPageNumber, destinationX, destinationY, std::string(), false);
}

Result createExternalLink(PDFDoc *document, int sourcePageNumber, double linkLeft, double linkTop, double linkRight, double linkBottom, const std::string &url)
{
    return createLink(document, sourcePageNumber, linkLeft, linkTop, linkRight, linkBottom, std::string(), 0, 0.0, 0.0, url, true);
}

Result editLinkRectangle(PDFDoc *document,
                         int sourcePageNumber,
                         double oldLinkLeft,
                         double oldLinkTop,
                         double oldLinkRight,
                         double oldLinkBottom,
                         double newLinkLeft,
                         double newLinkTop,
                         double newLinkRight,
                         double newLinkBottom)
{
    if (!document) {
        return makeError(Error::InvalidArguments, "The PDF document is not open.");
    }
    const int pageCount = document->getNumPages();
    const auto validRectangle = [](double left, double top, double right, double bottom) { return left >= 0.0 && top >= 0.0 && right <= 1.0 && bottom <= 1.0 && left < right && top < bottom; };
    if (sourcePageNumber < 1 || sourcePageNumber > pageCount || !validRectangle(oldLinkLeft, oldLinkTop, oldLinkRight, oldLinkBottom) || !validRectangle(newLinkLeft, newLinkTop, newLinkRight, newLinkBottom)) {
        return makeError(Error::InvalidArguments, "The link rectangle is invalid.", pageCount);
    }

    Page *page = document->getPage(sourcePageNumber);
    if (!page) {
        return makeError(Error::PageError, "Could not read the link page.", pageCount);
    }

    const NormalizedLinkRectangle requested { oldLinkLeft, oldLinkTop, oldLinkRight, oldLinkBottom };
    std::shared_ptr<Annot> selectedLink;
    double selectedDistance = std::numeric_limits<double>::max();
    for (const std::shared_ptr<Annot> &annotation : page->getAnnots()->getAnnots()) {
        if (!annotation || annotation->getType() != Annot::typeLink) {
            continue;
        }
        const double distance = linkRectangleDistance(requested, normalizedLinkRectangle(page, annotation->getRect()));
        if (distance < selectedDistance) {
            selectedDistance = distance;
            selectedLink = annotation;
        }
    }
    if (!selectedLink || selectedDistance > 0.04) {
        return makeError(Error::PageError, "Could not find the selected link.", pageCount);
    }

    double firstX = 0.0;
    double firstY = 0.0;
    double secondX = 0.0;
    double secondY = 0.0;
    if (!normalizedPointToUserCoordinates(page, newLinkLeft, newLinkTop, &firstX, &firstY) || !normalizedPointToUserCoordinates(page, newLinkRight, newLinkBottom, &secondX, &secondY)) {
        return makeError(Error::InvalidArguments, "The new link rectangle is invalid.", pageCount);
    }

    selectedLink->setRect(PDFRectangle(std::min(firstX, secondX), std::min(firstY, secondY), std::max(firstX, secondX), std::max(firstY, secondY)));
    const Object &annotationObject = selectedLink->getAnnotObj();
    XRef *xref = document->getXRef();
    if (selectedLink->getHasRef()) {
        xref->setModifiedObject(&annotationObject, selectedLink->getRef());
    } else {
        const Object &annotsReference = page->getPageObj().dictLookupNF("Annots");
        if (annotsReference.isRef()) {
            Object annots = annotsReference.fetch(xref);
            xref->setModifiedObject(&annots, annotsReference.getRef());
        } else {
            xref->setModifiedObject(&page->getPageObj(), page->getRef());
        }
    }

    return { Error::None, std::string(), pageCount, pageCount };
}

Result deleteLink(PDFDoc *document, int sourcePageNumber, double linkLeft, double linkTop, double linkRight, double linkBottom)
{
    if (!document) {
        return makeError(Error::InvalidArguments, "The PDF document is not open.");
    }
    const int pageCount = document->getNumPages();
    if (sourcePageNumber < 1 || sourcePageNumber > pageCount || linkLeft < 0.0 || linkTop < 0.0 || linkRight > 1.0 || linkBottom > 1.0 || linkLeft > linkRight || linkTop > linkBottom) {
        return makeError(Error::InvalidArguments, "The source link position is invalid.", pageCount);
    }

    Page *page = document->getPage(sourcePageNumber);
    if (!page) {
        return makeError(Error::PageError, "Could not read the source link page.", pageCount);
    }

    const NormalizedLinkRectangle requested { linkLeft, linkTop, linkRight, linkBottom };
    std::shared_ptr<Annot> selectedLink;
    double selectedDistance = std::numeric_limits<double>::max();
    for (const std::shared_ptr<Annot> &annotation : page->getAnnots()->getAnnots()) {
        if (!annotation || annotation->getType() != Annot::typeLink) {
            continue;
        }
        const double distance = linkRectangleDistance(requested, normalizedLinkRectangle(page, annotation->getRect()));
        if (distance < selectedDistance) {
            selectedDistance = distance;
            selectedLink = annotation;
        }
    }
    if (!selectedLink || selectedDistance > 0.04) {
        return makeError(Error::PageError, "Could not find the selected link.", pageCount);
    }

    page->removeAnnot(selectedLink);
    return { Error::None, std::string(), pageCount, pageCount };
}

Result addOcrTextLayers(const std::string &inputFileName, const std::string &outputFileName, const std::vector<OcrPage> &pages)
{
    std::unique_ptr<PDFDoc> document;
    Result openResult = openEditablePdf(inputFileName, outputFileName, &document);
    if (!openResult.ok()) {
        return openResult;
    }

    const int pageCount = document->getNumPages();
    for (const OcrPage &ocrPage : pages) {
        if (ocrPage.pageNumber < 1 || ocrPage.pageNumber > pageCount) {
            return makeError(Error::InvalidArguments, "An OCR page number is outside the document page range.", pageCount);
        }
    }

    XRef *xref = document->getXRef();
    auto *font = new Dict(xref);
    font->add("Type", Object(objName, "Font"));
    font->add("Subtype", Object(objName, "Type1"));
    font->add("BaseFont", Object(objName, "Helvetica"));
    font->add("Encoding", Object(objName, "WinAnsiEncoding"));
    const Ref fontRef = xref->addIndirectObject(Object(font));

    const auto escapePdfLiteral = [](const std::string &text) {
        std::string escaped;
        escaped.reserve(text.size());
        for (const unsigned char character : text) {
            if (character == '\\' || character == '(' || character == ')') {
                escaped.push_back('\\');
            }
            if (character >= 0x20 && character <= 0x7e) {
                escaped.push_back(static_cast<char>(character));
            }
        }
        return escaped;
    };

    for (const OcrPage &ocrPage : pages) {
        Page *page = document->getPage(ocrPage.pageNumber);
        if (!page) {
            return makeError(Error::PageError, "Could not read an OCR page.", pageCount);
        }

        std::ostringstream contents;
        contents.setf(std::ios::fixed);
        contents.precision(6);
        contents << "q\nBT\n3 Tr\n";

        int validWordCount = 0;
        for (const OcrWord &word : ocrPage.words) {
            const std::string text = escapePdfLiteral(word.text);
            if (text.empty() || word.left < 0.0 || word.top < 0.0 || word.right > 1.0 || word.bottom > 1.0 || word.left >= word.right || word.top >= word.bottom) {
                continue;
            }

            double originX = 0.0;
            double originY = 0.0;
            double rightX = 0.0;
            double rightY = 0.0;
            double topX = 0.0;
            double topY = 0.0;
            if (!normalizedPointToUserCoordinates(page, word.left, word.bottom, &originX, &originY)
                || !normalizedPointToUserCoordinates(page, word.right, word.bottom, &rightX, &rightY)
                || !normalizedPointToUserCoordinates(page, word.left, word.top, &topX, &topY)) {
                continue;
            }

            // Helvetica is only a metrics carrier here: rendering mode 3 keeps
            // the text invisible. Scale its baseline and cap height to the OCR
            // word box so standard PDF extractors recover both text and layout.
            const double nominalWidth = std::max(0.5, static_cast<double>(text.size()) * 0.5);
            constexpr double nominalHeight = 0.72;
            const double a = (rightX - originX) / nominalWidth;
            const double b = (rightY - originY) / nominalWidth;
            const double c = (topX - originX) / nominalHeight;
            const double d = (topY - originY) / nominalHeight;
            contents << "/MengsheeOCR 1 Tf\n" << a << ' ' << b << ' ' << c << ' ' << d << ' ' << originX << ' ' << originY << " Tm\n(" << text << ") Tj\n";
            ++validWordCount;
        }
        contents << "ET\nQ\n";
        if (validWordCount == 0) {
            continue;
        }

        const std::string contentText = contents.str();
        std::vector<char> contentBytes(contentText.begin(), contentText.end());
        const Ref contentRef = xref->addStreamObject(new Dict(xref), std::move(contentBytes), StreamCompression::Compress);

        Object pageObject = page->getPageObj().copy();
        const Object &oldContents = pageObject.dictLookupNF("Contents");
        auto *contentArray = new Array(xref);
        if (oldContents.isArray()) {
            const Array *oldArray = oldContents.getArray();
            for (int i = 0; i < oldArray->getLength(); ++i) {
                contentArray->add(oldArray->getNF(i).copy());
            }
        } else if (!oldContents.isNull()) {
            contentArray->add(oldContents.copy());
        }
        contentArray->add(Object(contentRef));
        pageObject.dictSet("Contents", Object(contentArray));

        std::unique_ptr<Dict> resources(page->getResourceDictCopy(xref));
        if (!resources) {
            resources = std::make_unique<Dict>(xref);
        }
        Object fontResources = resources->lookup("Font");
        std::unique_ptr<Dict> fonts(fontResources.isDict() ? fontResources.getDict()->copy(xref) : new Dict(xref));
        fonts->set("MengsheeOCR", Object(fontRef));
        resources->set("Font", Object(fonts.release()));
        pageObject.dictSet("Resources", Object(resources.release()));
        xref->setModifiedObject(&pageObject, page->getRef());
    }

    return saveEditedPdf(document.get(), outputFileName);
}

}
