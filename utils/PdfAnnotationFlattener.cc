//========================================================================
// PdfAnnotationFlattener.cc
// This file is licensed under the GPLv2 or later
//========================================================================
#include "PdfAnnotationFlattener.h"
#include "config.h"
#include <poppler-config.h>

#include "Annot.h"
#include "Array.h"
#include "Dict.h"
#include "ErrorCodes.h"
#include "Gfx.h"
#include "GlobalParams.h"
#include "Object.h"
#include "OutputDev.h"
#include "PDFDoc.h"
#include "Page.h"
#include "Stream.h"
#include "XRef.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using PdfAnnotationFlattener::Error;
using PdfAnnotationFlattener::Result;
using AppearanceMatrix = std::array<double, 6>;
namespace fs = std::filesystem;

struct Failure
{
    Error error;
    std::string message;
};
[[noreturn]] void fail(Error error, const std::string &message) { throw Failure { error, message }; }

// A PDF number cannot use exponent notation. Classic locale avoids decimal commas.
std::string number(double value)
{
    if (!std::isfinite(value) || std::abs(value) > 1e12) {
        fail(Error::InvalidAppearance, "Appearance transform is non-finite or outside the supported numeric range.");
    }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    const int precision = 17 + (value != 0 && std::abs(value) < 1 ? static_cast<int>(std::ceil(-std::log10(std::abs(value)))) : 0);
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}
std::string matrixCommand(const AppearanceMatrix &m)
{
    std::string result;
    for (double v : m) {
        result += number(v) + " ";
    }
    return result + "cm\n";
}

template<size_t N> std::array<double, N> numbers(const Object &obj, const char *description)
{
    if (!obj.isArray() || obj.arrayGetLength() != N) {
        fail(Error::InvalidAppearance, std::string("Invalid ") + description + ".");
    }
    std::array<double, N> values;
    for (size_t i = 0; i < N; ++i) {
        Object n = obj.arrayGet(static_cast<int>(i));
        if (!n.isNum() || !std::isfinite(n.getNum())) {
            fail(Error::InvalidAppearance, std::string("Non-numeric ") + description + ".");
        }
        values[i] = n.getNum();
    }
    return values;
}

AppearanceMatrix appearanceMatrix(Object &appearance)
{
    if (!appearance.isStream() || !appearance.streamGetDict()->lookup("Subtype").isName("Form")) {
        fail(Error::InvalidAppearance, "Normal appearance must be a Form XObject stream.");
    }
    Dict *d = appearance.streamGetDict();
    if (d->hasKey("OC")) {
        fail(Error::UnsupportedAnnotation, "Optional-content appearances cannot be flattened safely.");
    }
    Object resources = d->lookup("Resources");
    if (!resources.isNull() && !resources.isDict()) {
        fail(Error::InvalidAppearance, "Invalid appearance resources.");
    }
    Object m = d->lookup("Matrix");
    AppearanceMatrix matrix = m.isNull() ? AppearanceMatrix { 1, 0, 0, 1, 0, 0 } : numbers<6>(m, "appearance Matrix");
    const double det = matrix[0] * matrix[3] - matrix[1] * matrix[2];
    if (!std::isfinite(det) || std::abs(det) < 1e-15) {
        fail(Error::InvalidAppearance, "Singular appearance Matrix.");
    }
    return matrix;
}

AppearanceMatrix placement(Object &appearance, const std::array<double, 4> &rect, const AppearanceMatrix &m)
{
    const auto b = numbers<4>(appearance.streamGetDict()->lookup("BBox"), "appearance BBox");
    if (b[0] >= b[2] || b[1] >= b[3] || rect[0] >= rect[2] || rect[1] >= rect[3]) {
        fail(Error::InvalidAppearance, "Empty or inverted appearance BBox/annotation Rect.");
    }
    double xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    for (int i = 0; i < 4; ++i) {
        const double x = b[(i & 1) ? 2 : 0], y = b[(i & 2) ? 3 : 1];
        const double xx = m[0] * x + m[2] * y + m[4], yy = m[1] * x + m[3] * y + m[5];
        if (i == 0) {
            xmin = xmax = xx;
            ymin = ymax = yy;
        } else {
            xmin = std::min(xmin, xx);
            xmax = std::max(xmax, xx);
            ymin = std::min(ymin, yy);
            ymax = std::max(ymax, yy);
        }
    }
    if (!(xmax > xmin && ymax > ymin)) {
        fail(Error::InvalidAppearance, "Degenerate transformed appearance BBox.");
    }
    const double sx = (rect[2] - rect[0]) / (xmax - xmin), sy = (rect[3] - rect[1]) / (ymax - ymin);
    return { sx, 0, 0, sy, rect[0] - xmin * sx, rect[1] - ymin * sy };
}

std::array<double, 4> paintedBounds(Object &appearance, const AppearanceMatrix &mapping)
{
    const auto bbox = numbers<4>(appearance.streamGetDict()->lookup("BBox"), "appearance BBox");
    const AppearanceMatrix form = appearanceMatrix(appearance);
    std::array<double, 4> bounds {};
    for (int corner = 0; corner < 4; ++corner) {
        const double x = bbox[(corner & 1) ? 2 : 0], y = bbox[(corner & 2) ? 3 : 1];
        const double fx = form[0] * x + form[2] * y + form[4], fy = form[1] * x + form[3] * y + form[5];
        const double px = mapping[0] * fx + mapping[2] * fy + mapping[4], py = mapping[1] * fx + mapping[3] * fy + mapping[5];
        number(px);
        number(py);
        if (corner == 0) bounds = { px, py, px, py };
        else {
            bounds[0] = std::min(bounds[0], px);
            bounds[1] = std::min(bounds[1], py);
            bounds[2] = std::max(bounds[2], px);
            bounds[3] = std::max(bounds[3], py);
        }
    }
    return bounds;
}

void checkRetainedOverlap(Object &annotations, int index, Object &appearance, const AppearanceMatrix &mapping)
{
    const auto painted = paintedBounds(appearance, mapping);
    // Earlier retained annotations formerly painted BELOW this appearance.
    // Moving it into Contents would invert that ordering. Later ones remain above.
    for (int i = 0; i < index; ++i) {
        Object retained = annotations.arrayGet(i);
        Object subtype = retained.dictLookup("Subtype");
        if (!subtype.isName("Widget") && !subtype.isName("Link")) continue;
        Object flagObject = retained.dictLookup("F");
        const unsigned flags = flagObject.isInt() ? static_cast<unsigned>(flagObject.getInt()) : 0;
        if (flags & (1 | 2 | 32)) continue;
        if (flags & (8 | 16 | 256)) {
            fail(Error::UnsupportedAnnotation, "An earlier retained Link/Widget has dynamic view bounds; flattening cannot guarantee its stacking order.");
        }
        auto rect = numbers<4>(retained.dictLookup("Rect"), "retained Link/Widget Rect");
        if (rect[0] > rect[2]) std::swap(rect[0], rect[2]);
        if (rect[1] > rect[3]) std::swap(rect[1], rect[3]);
        if (painted[0] < rect[2] && painted[2] > rect[0] && painted[1] < rect[3] && painted[3] > rect[1]) {
            fail(Error::UnsupportedAnnotation, "Flattening would change the stacking order of an overlapping retained Link/Widget.");
        }
    }
}

AppearanceMatrix compose(const AppearanceMatrix &outer, const AppearanceMatrix &inner)
{
    return { outer[0] * inner[0] + outer[2] * inner[1], outer[1] * inner[0] + outer[3] * inner[1],
             outer[0] * inner[2] + outer[2] * inner[3], outer[1] * inner[2] + outer[3] * inner[3],
             outer[0] * inner[4] + outer[2] * inner[5] + outer[4], outer[1] * inner[4] + outer[3] * inner[5] + outer[5] };
}

AppearanceMatrix compensateNoRotate(const AppearanceMatrix &mapping, const std::array<double, 4> &rect, int pageRotation)
{
    // Match Gfx::drawAnnot: undo native page rotation about the top-left
    // annotation corner, in default PDF user space. /Rotate itself is retained.
    const double angle = ((360 - pageRotation) % 360) * std::acos(-1.0) / 180.0;
    const double c = std::cos(angle), s = std::sin(angle), x = rect[0], y = rect[3];
    const AppearanceMatrix rotation { c, -s, s, c, -c * x - s * y + x, -c * y + s * x + y };
    return compose(rotation, mapping);
}

// Capture Gfx::drawForm's actual placement for generated appearances. Generated
// line/ink/callout bounds need not equal /Rect. No private ABI accessor is needed.
class AppearanceOutput final : public OutputDev
{
public:
    bool upsideDown() override { return false; }
    bool useDrawChar() override { return false; }
    bool interpretType3Chars() override { return false; }
    void updateCTM(GfxState *, double a, double b, double c, double d, double e, double f) override
    {
        if (armed && !captured) {
            matrix = compose(matrix, AppearanceMatrix { a, b, c, d, e, f });
            sawTransform = true;
        }
    }
    void clip(GfxState *) override
    {
        // Gfx::drawForm clips to BBox after its placement updateCTM and before
        // interpreting AP operators. Freeze here: include NoRotate + Form,
        // exclude all inner AP cm operators and nested Forms.
        if (armed && sawTransform && !captured) captured = true;
    }
    bool armed = false;
    bool captured = false;
    bool sawTransform = false;
    AppearanceMatrix matrix { 1, 0, 0, 1, 0, 0 };
};

std::unique_ptr<Annot> makeAnnotation(PDFDoc *doc, Object &dict, const Object &reference, const std::string &type)
{
    if (type == "FreeText") return std::make_unique<AnnotFreeText>(doc, dict.copy(), &reference);
    if (type == "Line") return std::make_unique<AnnotLine>(doc, dict.copy(), &reference);
    if (type == "Square" || type == "Circle") return std::make_unique<AnnotGeometry>(doc, dict.copy(), &reference);
    if (type == "Polygon" || type == "PolyLine") return std::make_unique<AnnotPolygon>(doc, dict.copy(), &reference);
    if (type == "Ink") return std::make_unique<AnnotInk>(doc, dict.copy(), &reference);
    if (type == "Stamp") return std::make_unique<AnnotStamp>(doc, dict.copy(), &reference);
    if (type == "Highlight" || type == "Underline" || type == "Squiggly" || type == "StrikeOut") return std::make_unique<AnnotTextMarkup>(doc, dict.copy(), &reference);
    return nullptr;
}

AppearanceMatrix removeFormMatrix(const AppearanceMatrix &c, const AppearanceMatrix &m)
{
    const double det = m[0] * m[3] - m[1] * m[2];
    AppearanceMatrix inverse { m[3] / det, -m[1] / det, -m[2] / det, m[0] / det, (m[2] * m[5] - m[3] * m[4]) / det, (m[1] * m[4] - m[0] * m[5]) / det };
    return { c[0] * inverse[0] + c[2] * inverse[1], c[1] * inverse[0] + c[3] * inverse[1], c[0] * inverse[2] + c[2] * inverse[3], c[1] * inverse[2] + c[3] * inverse[3],
             c[0] * inverse[4] + c[2] * inverse[5] + c[4], c[1] * inverse[4] + c[3] * inverse[5] + c[5] };
}

// Inspect each xref object and its DIRECT children; indirect references are
// visited by the outer xref loop, avoiding graph cycles and inherited field traps.
void checkSignatures(const Object &obj, int depth, size_t &budget)
{
    if (!budget-- || depth > 128) {
        fail(Error::DamagedInput, "PDF object nesting/size exceeds signature preflight limits.");
    }
    if (obj.isRef()) return;
    if (obj.isArray()) {
        for (int i = 0; i < obj.arrayGetLength(); ++i) checkSignatures(obj.arrayGetNF(i), depth + 1, budget);
    } else if (obj.isDict() || obj.isStream()) {
        Dict *d = obj.isDict() ? obj.getDict() : obj.streamGetDict();
        if (d->hasKey("ByteRange") || d->lookup("Type").isName("Sig") || d->lookup("Type").isName("DocTimeStamp") || (d->lookup("FT").isName("Sig") && !d->lookup("V").isNull())) {
            fail(Error::SignedInput, "Signed PDFs/signature dictionaries cannot be flattened without invalidating signatures.");
        }
        for (int i = 0; i < d->getLength(); ++i) checkSignatures(d->getValNF(i), depth + 1, budget);
    }
}

// Core appearance generators can build nested direct memory streams (e.g.
// translucent markup's /Fm0). PDF syntax requires every stream to be indirect;
// PDFDoc::writeObject does not lift these for us. Preserve bytes/filters and lift
// only direct streams, leaving existing indirect resource graphs untouched.
bool indirectStreams(Object &object, XRef *xref, int depth, size_t &budget)
{
    if (!budget-- || depth > 128) fail(Error::InvalidAppearance, "Generated appearance resource graph exceeds limits.");
    bool changed = false;
    if (object.isArray()) {
        Object replacement(new Array(xref));
        for (int i = 0; i < object.arrayGetLength(); ++i) {
            Object child = object.arrayGetNF(i).copy();
            if (!child.isRef()) {
                changed = indirectStreams(child, xref, depth + 1, budget) || changed;
                if (child.isStream()) {
                    child = Object(xref->addIndirectObject(child));
                    changed = true;
                }
            }
            replacement.arrayAdd(std::move(child));
        }
        if (changed) object = std::move(replacement);
    } else if (object.isDict() || object.isStream()) {
        Dict *dict = object.isDict() ? object.getDict() : object.streamGetDict();
        std::vector<std::string> keys;
        for (int i = 0; i < dict->getLength(); ++i) keys.emplace_back(dict->getKey(i));
        for (const std::string &key : keys) {
            Object child = dict->lookupNF(key).copy();
            if (child.isRef()) continue;
            bool nested = indirectStreams(child, xref, depth + 1, budget);
            if (child.isStream()) {
                child = Object(xref->addIndirectObject(child));
                nested = true;
            }
            if (nested) {
                dict->set(key, std::move(child));
                changed = true;
            }
        }
    }
    return changed;
}

bool supported(const std::string &t)
{
    return t == "FreeText" || t == "Line" || t == "Square" || t == "Circle" || t == "Polygon" || t == "PolyLine" || t == "Highlight" || t == "Underline" || t == "Squiggly" || t == "StrikeOut" || t == "Ink" || t == "Stamp" || t == "Text" || t == "Caret";
}

Ref stream(XRef *xref, const std::string &text)
{
    return xref->addStreamObject(new Dict(xref), std::vector<char>(text.begin(), text.end()), StreamCompression::Compress);
}

struct TemporaryDirectory
{
    fs::path path;
    ~TemporaryDirectory() { if (!path.empty()) { std::error_code ignored; fs::remove_all(path, ignored); } }
};

} // namespace

namespace PdfAnnotationFlattener {
Result flatten(const std::string &inputFileName, const std::string &outputFileName)
{
    Result result;
    try {
        if (inputFileName.empty() || outputFileName.empty()) fail(Error::InvalidArguments, "Input and output paths must be non-empty.");
        const fs::path input = fs::u8path(inputFileName), output = fs::u8path(outputFileName);
        // symlink_status also detects dangling symlinks. Never overwrite a caller file.
        std::error_code ec;
        const auto status = fs::symlink_status(output, ec);
        if (fs::exists(status)) fail(Error::InvalidArguments, "Output must be a new file; an existing output will not be overwritten.");
        if (ec && ec != std::errc::no_such_file_or_directory) fail(Error::IoError, "Cannot inspect output path: " + ec.message());
        if (fs::weakly_canonical(input) == fs::weakly_canonical(output)) fail(Error::InvalidArguments, "Input and output paths must differ.");
        if (!globalParams) globalParams = std::make_unique<GlobalParams>();
        PDFDoc doc(std::make_unique<GooString>(inputFileName));
        if (!doc.isOk()) fail(doc.getErrorCode() == errEncrypted ? Error::EncryptedInput : Error::DamagedInput, "Cannot open input PDF.");
        if (doc.isEncrypted()) fail(Error::EncryptedInput, "Encrypted PDFs are not supported.");
        result.pageCount = doc.getNumPages();
        XRef *xref = doc.getXRef();
        size_t budget = 2000000;
        const int originalObjectCount = xref->getNumObjects();
        for (int i = 0; i < originalObjectCount; ++i) {
            const XRefEntry *entry = xref->getEntry(i);
            if (!entry || entry->type == xrefEntryFree || entry->type == xrefEntryNone) continue;
            Object object = xref->fetch(Ref { i, entry->type == xrefEntryCompressed ? 0 : entry->gen });
            checkSignatures(object, 0, budget);
        }

        std::set<std::pair<int, int>> removedReferences;
        for (int pageNo = 1; pageNo <= result.pageCount; ++pageNo) {
            result.pageNumber = pageNo;
            result.annotationIndex = 0;
            Page *page = doc.getPage(pageNo);
            if (!page || !page->isOk()) fail(Error::DamagedInput, "Invalid page.");
            Object pageObject = xref->fetch(page->getRef());
            if (!pageObject.isDict()) fail(Error::DamagedInput, "Invalid page dictionary.");
            Object annotations = pageObject.dictLookup("Annots");
            if (annotations.isNull()) continue;
            if (!annotations.isArray()) fail(Error::DamagedInput, "Page Annots must be an array.");
            Object resources(page->getResourceDict() ? page->getResourceDict()->copy(xref) : new Dict(xref));
            Object oldXObjects = resources.dictLookup("XObject");
            if (!oldXObjects.isNull() && !oldXObjects.isDict()) fail(Error::DamagedInput, "Invalid page XObject resources.");
            Object xobjects(oldXObjects.isDict() ? oldXObjects.getDict()->copy(xref) : new Dict(xref));
            std::string commands = "Q\n"; // Completes the prefix q around original content.
            std::set<int> removed;
            const int count = annotations.arrayGetLength();
            for (int index = 0; index < count; ++index) {
                result.annotationIndex = index + 1;
                Object annotation = annotations.arrayGet(index);
                if (!annotation.isDict()) fail(Error::DamagedInput, "Invalid annotation dictionary.");
                Object subtype = annotation.dictLookup("Subtype");
                if (!subtype.isName()) fail(Error::UnsupportedAnnotation, "Annotation has no valid Subtype.");
                const std::string type = subtype.getName();
                Object flagsObject = annotation.dictLookup("F");
                if (!flagsObject.isNull() && !flagsObject.isInt()) fail(Error::DamagedInput, "Invalid annotation flags.");
                const unsigned flags = flagsObject.isInt() ? static_cast<unsigned>(flagsObject.getInt()) : 0;
                if (type == "Popup") continue; // Resolve paired popups after the parent decisions.
                if (type == "Link" || type == "Widget" || (flags & (1 | 2 | 32))) {
                    ++result.preservedAnnotations;
                    if (flags & (1 | 2 | 32)) ++result.preservedHiddenAnnotations;
                    continue;
                }
                if (!supported(type)) fail(Error::UnsupportedAnnotation, "Unsupported visible annotation type: " + type + ".");
                if (flags & (8 | 256)) fail(Error::UnsupportedAnnotation, "NoZoom and ToggleNoView annotations cannot be flattened safely.");
                for (const char *key : { "IRT", "RT", "OC", "A", "AA", "StructParent", "ExData" }) {
                    if (annotation.getDict()->hasKey(key)) fail(Error::UnsupportedAnnotation, std::string("Annotation feature cannot be flattened safely: ") + key + ".");
                }
                if (annotation.getDict()->hasKey("Popup")) {
                    const Object &popup = annotation.dictLookupNF("Popup");
                    bool found = false;
                    for (int j = 0; popup.isRef() && j < count; ++j) {
                        const Object &candidate = annotations.arrayGetNF(j);
                        if (candidate.isRef() && candidate.getRef() == popup.getRef()) {
                            Object popupObject = annotations.arrayGet(j);
                            found = popupObject.isDict() && popupObject.dictLookup("Subtype").isName("Popup");
                            break;
                        }
                    }
                    if (!found) fail(Error::UnsupportedAnnotation, "Popup must be an indirect paired annotation on the same page.");
                }
                auto rect = numbers<4>(annotation.dictLookup("Rect"), "annotation Rect");
                if (rect[0] > rect[2]) std::swap(rect[0], rect[2]);
                if (rect[1] > rect[3]) std::swap(rect[1], rect[3]);
                if (!(rect[2] > rect[0] && rect[3] > rect[1])) fail(Error::InvalidAppearance, "Empty annotation Rect.");
                for (double coordinate : rect) number(coordinate); // Bound values before invoking Core generators.
                Object ap = annotation.dictLookup("AP");
                if (!ap.isNull() && !ap.isDict()) fail(Error::InvalidAppearance, "Invalid AP dictionary.");
                Object appearance = ap.isDict() ? ap.dictLookup("N") : Object::null();
                if (appearance.isDict()) {
                    Object state = annotation.dictLookup("AS");
                    if (!state.isName()) fail(Error::InvalidAppearance, "Normal appearance state dictionary requires an explicit AS name.");
                    appearance = appearance.dictLookup(state.getName());
                    if (!appearance.isStream()) fail(Error::InvalidAppearance, "AS does not select a normal appearance stream.");
                }
                AppearanceMatrix mapping;
                if (appearance.isNull()) {
                    // Annot::setPage is private. A private dictionary copy with
                    // an explicit /P makes initialize() resolve this page even if
                    // the source omitted /P (getRotation() always needs a page).
                    Object generationDictionary(annotation.getDict()->copy(xref));
                    generationDictionary.dictSet("P", Object(page->getRef()));
                    auto annot = makeAnnotation(&doc, generationDictionary, annotations.arrayGetNF(index), type);
                    if (!annot || !annot->isOk()) fail(Error::InvalidAppearance, "Missing normal appearance cannot be generated for this annotation.");
                    AppearanceOutput device;
                    Gfx gfx(&doc, &device, pageNo, page->getResourceDict(), 72, 72, page->getMediaBox(), page->getCropBox(), page->getRotate());
                    device.armed = true;
                    annot->draw(&gfx, false);
                    appearance = annot->getAppearance();
                    if (!device.captured || !appearance.isStream()) fail(Error::InvalidAppearance, "Poppler could not generate a usable vector appearance.");
                    const AppearanceMatrix m = appearanceMatrix(appearance);
                    // Validate the bounds as well as the captured transform.
                    placement(appearance, rect, m);
                    mapping = removeFormMatrix(device.matrix, m);
                    ++result.generatedAppearances;
                } else {
                    mapping = placement(appearance, rect, appearanceMatrix(appearance));
                    if (flags & 16) mapping = compensateNoRotate(mapping, rect, page->getRotate());
                }
                checkRetainedOverlap(annotations, index, appearance, mapping);
                const std::string name = xobjects.getDict()->findAvailableKey("FlatAnnot");
                xobjects.dictSet(name, Object(xref->addIndirectObject(appearance)));
                // Existing AP owns its alpha, blend mode, clipping and resources.
                // /Rotate affects both page content and ordinary annotations alike.
                commands += "q\n" + matrixCommand(mapping) + "/" + name + " Do\nQ\n";
                removed.insert(index);
                ++result.flattenedAnnotations;
            }

            // Only remove ordinary paired Popup UI containers with their flattened
            // parent. Unknown/orphan/interactive popups and replies fail closed.
            for (int index = 0; index < count; ++index) {
                result.annotationIndex = index + 1;
                Object a = annotations.arrayGet(index);
                if (!a.dictLookup("Subtype").isName("Popup")) continue;
                const Object &parent = a.dictLookupNF("Parent");
                int parentIndex = -1;
                for (int j = 0; j < count && parent.isRef(); ++j) {
                    const Object &candidate = annotations.arrayGetNF(j);
                    if (candidate.isRef() && candidate.getRef() == parent.getRef()) { parentIndex = j; break; }
                }
                if (parentIndex < 0) fail(Error::UnsupportedAnnotation, "Orphan or direct-parent Popup is unsupported.");
                if (!removed.count(parentIndex)) {
                    ++result.preservedAnnotations;
                    Object popupFlags = a.dictLookup("F");
                    if (popupFlags.isInt() && (popupFlags.getInt() & (1 | 2 | 32))) ++result.preservedHiddenAnnotations;
                    continue;
                }
                Object parentObject = annotations.arrayGet(parentIndex);
                const Object &back = parentObject.dictLookupNF("Popup");
                const Object &popupRef = annotations.arrayGetNF(index);
                if (!back.isRef() || !popupRef.isRef() || back.getRef() != popupRef.getRef()) fail(Error::UnsupportedAnnotation, "Popup parent association is not reciprocal.");
                for (const char *key : { "IRT", "RT", "OC", "A", "AA", "StructParent", "Contents" }) {
                    if (a.getDict()->hasKey(key)) fail(Error::UnsupportedAnnotation, "Popup has independent content or behavior.");
                }
                removed.insert(index);
                ++result.removedPopupAnnotations;
            }
            // Retained replies must never be left pointing at a removed parent.
            for (int index = 0; index < count; ++index) {
                Object a = annotations.arrayGet(index);
                const Object &reply = a.dictLookupNF("IRT");
                if (!reply.isRef()) continue;
                for (int j : removed) {
                    const Object &r = annotations.arrayGetNF(j);
                    if (r.isRef() && r.getRef() == reply.getRef()) fail(Error::UnsupportedAnnotation, "A retained reply references a flattened annotation.");
                }
            }
            if (removed.empty()) continue;
            for (int index : removed) {
                const Object &reference = annotations.arrayGetNF(index);
                if (reference.isRef()) removedReferences.emplace(reference.getRef().num, reference.getRef().gen);
            }
            Object survivors(new Array(xref));
            for (int i = 0; i < count; ++i) if (!removed.count(i)) survivors.arrayAdd(annotations.arrayGetNF(i).copy());
            Object contents(new Array(xref));
            contents.arrayAdd(Object(stream(xref, "q\n")));
            Object oldContents = pageObject.dictLookup("Contents");
            if (oldContents.isArray()) {
                for (int i = 0; i < oldContents.arrayGetLength(); ++i) {
                    if (!oldContents.arrayGet(i).isStream()) fail(Error::DamagedInput, "Invalid page Contents array.");
                    contents.arrayAdd(oldContents.arrayGetNF(i).copy());
                }
            } else if (oldContents.isStream()) {
                contents.arrayAdd(pageObject.dictLookupNF("Contents").copy());
            } else if (!oldContents.isNull()) {
                fail(Error::DamagedInput, "Invalid page Contents.");
            }
            contents.arrayAdd(Object(stream(xref, commands)));
            resources.dictSet("XObject", std::move(xobjects));
            pageObject.dictSet("Resources", std::move(resources));
            pageObject.dictSet("Contents", std::move(contents));
            pageObject.dictSet("Annots", std::move(survivors));
            xref->setModifiedObject(&pageObject, page->getRef());
        }
        // /IRT may target an annotation on a different page. Inspect the final
        // retained arrays, not Page's cached original annotations.
        for (int pageNo = 1; pageNo <= result.pageCount; ++pageNo) {
            result.pageNumber = pageNo;
            Object pageObject = xref->fetch(doc.getPage(pageNo)->getRef());
            Object annotations = pageObject.dictLookup("Annots");
            if (!annotations.isArray()) continue;
            for (int index = 0; index < annotations.arrayGetLength(); ++index) {
                result.annotationIndex = index + 1;
                Object annotation = annotations.arrayGet(index);
                const Object &reply = annotation.dictLookupNF("IRT");
                if (reply.isRef() && removedReferences.count({ reply.getRef().num, reply.getRef().gen })) {
                    fail(Error::UnsupportedAnnotation, "A retained reply references a flattened annotation on another page.");
                }
            }
        }
        result.pageNumber = result.annotationIndex = 0;
        budget = 2000000;
        for (int i = 0; i < xref->getNumObjects(); ++i) {
            const XRefEntry *entry = xref->getEntry(i);
            if (!entry || entry->type == xrefEntryFree || !entry->getFlag(XRefEntry::Updated)) continue;
            const Ref ref { i, entry->type == xrefEntryCompressed ? 0 : entry->gen };
            Object object = xref->fetch(ref);
            if (indirectStreams(object, xref, 0, budget)) xref->setModifiedObject(&object, ref);
        }
        TemporaryDirectory temporary;
        std::random_device random;
        for (int attempt = 0; attempt < 32 && temporary.path.empty(); ++attempt) {
            fs::path candidate = output;
            candidate += ".flatten-" + std::to_string(random()) + ".tmp";
            ec.clear();
            if (fs::create_directory(candidate, ec)) temporary.path = std::move(candidate);
            else if (ec && ec != std::errc::file_exists) fail(Error::IoError, "Cannot create output temporary directory: " + ec.message());
        }
        if (temporary.path.empty()) fail(Error::IoError, "Cannot reserve output temporary directory.");
        const fs::path temporaryFile = temporary.path / "output.pdf";
        const auto encoded = temporaryFile.u8string();
        const std::string temporaryName(encoded.begin(), encoded.end());
        if (doc.saveAs(temporaryName, writeForceRewrite) != errNone) fail(Error::WriteError, "Could not write flattened PDF.");
        {
            PDFDoc verification(std::make_unique<GooString>(temporaryName));
            if (!verification.isOk() || verification.getNumPages() != result.pageCount) fail(Error::WriteError, "Flattened PDF failed reopen validation.");
            for (int i = 1; i <= result.pageCount; ++i) {
                Page *verified = verification.getPage(i);
                if (!verified || !verified->isOk()) fail(Error::WriteError, "Flattened PDF contains an unreadable page.");
            }
        }
        // Atomic no-replace publication. Unlike rename, this never replaces an
        // existing input alias, even if another process creates it during export.
        fs::create_hard_link(temporaryFile, output, ec);
        if (ec) fail(Error::IoError, "Cannot publish output without replacement (hard-link support required): " + ec.message());
    } catch (const Failure &failure) {
        result.error = failure.error;
        result.message = failure.message;
    } catch (const std::exception &exception) {
        result.error = Error::IoError;
        result.message = exception.what();
    }
    if (!result.ok()) {
        result.flattenedAnnotations = result.generatedAppearances = result.removedPopupAnnotations = 0;
        result.preservedAnnotations = result.preservedHiddenAnnotations = 0;
    }
    return result;
}
} // namespace PdfAnnotationFlattener
