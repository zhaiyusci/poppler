//========================================================================
// PdfPageBounds.cc -- single-page appearance bounds, GPLv2 or later
//========================================================================
#include "PdfPageBounds.h"
#include "config.h"
#include <poppler-config.h>
#include "Array.h"
#include "Catalog.h"
#include "Dict.h"
#include "ErrorCodes.h"
#include "Gfx.h"
#include "GfxFont.h"
#include "GfxState.h"
#include "GlobalParams.h"
#include "Object.h"
#include "PDFDoc.h"
#include "Page.h"
#include "SplashOutputDev.h"
#include "Stream.h"
#include "XRef.h"
#include "splash/SplashFont.h"
#include "splash/SplashPath.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using PdfPageBounds::Error;
using PdfPageBounds::Rect;
namespace fs = std::filesystem;
constexpr double inspectionLimit = 1000000;
constexpr double spanLimit = 50000;
constexpr size_t byteLimit = 64 * 1024 * 1024;
constexpr size_t pointLimit = 2000000;
struct Failure
{
    Error error;
    std::string message;
};
[[noreturn]] void fail(Error e, const std::string &m)
{
    throw Failure { e, m };
}
struct Budget
{
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    size_t points = 0, bytes = 0, objects = 0;
    bool expired = false;
    bool abort()
    {
        expired = expired || std::chrono::steady_clock::now() >= end;
        return expired;
    }
    void check()
    {
        if (abort())
            fail(Error::LimitExceeded, "Appearance inspection exceeded the 15 second deadline.");
    }
    void point()
    {
        if (++points > pointLimit)
            fail(Error::LimitExceeded, "Appearance paint/point budget exceeded.");
        if ((points & 255) == 0)
            check();
    }
    static bool callback(void *p) { return static_cast<Budget *>(p)->abort(); }
};
void finite(double n)
{
    if (!std::isfinite(n) || std::abs(n) > 1e12)
        fail(Error::UnsupportedInput, "Non-finite or excessive appearance geometry.");
}
bool positive(const Rect &r)
{
    return r.x1 < r.x2 && r.y1 < r.y2;
}
Rect intersect(const Rect &a, const Rect &b)
{
    return { std::max(a.x1, b.x1), std::max(a.y1, b.y1), std::min(a.x2, b.x2), std::min(a.y2, b.y2) };
}
Rect unite(const Rect &a, const Rect &b)
{
    return { std::min(a.x1, b.x1), std::min(a.y1, b.y1), std::max(a.x2, b.x2), std::max(a.y2, b.y2) };
}
struct Box
{
    Rect rect;
    bool any = false;
    void add(double x, double y)
    {
        finite(x);
        finite(y);
        if (!any) {
            rect = { x, y, x, y };
            any = true;
        } else {
            rect.x1 = std::min(rect.x1, x);
            rect.y1 = std::min(rect.y1, y);
            rect.x2 = std::max(rect.x2, x);
            rect.y2 = std::max(rect.y2, y);
        }
    }
};

// Reuse Core's embedded-font/code->GID machinery, but never invoke painting.
// startPage(nullptr) allocates a constant 1x1 surface, not a page-size bitmap.
class FontProvider final : public SplashOutputDev
{
public:
    FontProvider() : SplashOutputDev(splashModeMono8, 1, nullptr, true) { setFreeTypeHinting(false, false); }
    SplashFont *prepare(GfxState *state)
    {
        updateCTM(state, 0, 0, 0, 0, 0, 0);
        doUpdateFont(state);
        return getCurrentFont();
    }
};

class InkDevice final : public OutputDev
{
    Budget &budget;
    FontProvider fonts;
    std::array<double, 6> inverse { 1, 0, 0, 1, 0, 0 };
    Rect clipBox { -inspectionLimit, -inspectionLimit, inspectionLimit, inspectionLimit };
    std::vector<Rect> clips;
    Rect ink;
    bool marked = false;
    int softMaskDepth = 0;
    std::vector<bool> groups;
    void toPdf(double &x, double &y) const
    {
        const double a = inverse[0] * x + inverse[2] * y + inverse[4];
        const double b = inverse[1] * x + inverse[3] * y + inverse[5];
        x = a;
        y = b;
        finite(x);
        finite(y);
    }
    void point(Box &box, GfxState *state, double x, double y)
    {
        budget.point();
        finite(x);
        finite(y);
        double a, b;
        state->transform(x, y, &a, &b);
        toPdf(a, b);
        if (std::abs(a) >= inspectionLimit - 1 || std::abs(b) >= inspectionLimit - 1)
            fail(Error::LimitExceeded, "Geometry exceeds the bounded inspection area.");
        box.add(a, b);
    }
    void commit(const Box &box)
    {
        if (!box.any || softMaskDepth)
            return;
        const Rect r = intersect(box.rect, clipBox);
        if (!positive(r))
            return; // Intersection, never clamping points to an edge.
        if (r.x1 <= -inspectionLimit + 1 || r.y1 <= -inspectionLimit + 1 || r.x2 >= inspectionLimit - 1 || r.y2 >= inspectionLimit - 1)
            fail(Error::LimitExceeded, "Paint reaches the bounded inspection area; bounds would be incomplete.");
        ink = marked ? unite(ink, r) : r;
        marked = true;
    }
    void widenStroke(Box &box, GfxState *state, bool joins)
    {
        if (!box.any)
            return;
        const auto &m = state->getCTM();
        for (double n : m)
            finite(n);
        double w = std::abs(state->getLineWidth());
        finite(w);
        // Device->PDF linear transform; account for shears/non-uniform scale.
        double a = inverse[0] * m[0] + inverse[2] * m[1], b = inverse[1] * m[0] + inverse[3] * m[1];
        double c = inverse[0] * m[2] + inverse[2] * m[3], d = inverse[1] * m[2] + inverse[3] * m[3];
        double factor = state->getLineCap() == 2 ? std::sqrt(2.0) : 1.0;
        if (joins && state->getLineJoin() == 0) {
            finite(state->getMiterLimit());
            factor = std::max(factor, std::max(1.0, state->getMiterLimit()));
        }
        // A PDF hairline is device-dependent: a conservative 1bp envelope.
        double px = w == 0 ? 0.5 : 0.5 * w * factor * std::hypot(a, c), py = w == 0 ? 0.5 : 0.5 * w * factor * std::hypot(b, d);
        finite(px);
        finite(py);
        box.rect.x1 -= px;
        box.rect.x2 += px;
        box.rect.y1 -= py;
        box.rect.y2 += py;
    }
    std::array<double, 2> glyphPrecision(GfxState *state)
    {
        const auto &tm = state->getTextMat();
        const auto &ctm = state->getCTM();
        const double size = state->getFontSize(), horizontal = state->getHorizScaling();
        std::array<double, 4> local { tm[0] * size * horizontal, tm[1] * size * horizontal, tm[2] * size, tm[3] * size };
        std::array<double, 4> device { local[0] * ctm[0] + local[1] * ctm[2], local[0] * ctm[1] + local[1] * ctm[3], local[2] * ctm[0] + local[3] * ctm[2], local[2] * ctm[1] + local[3] * ctm[3] };
        auto checkCondition = [](const std::array<double, 4> &m) {
            double scale = 0;
            for (double v : m) {
                finite(v);
                scale = std::max(scale, std::abs(v));
            }
            if (scale == 0)
                fail(Error::UnsupportedInput, "Singular text transform cannot be measured safely.");
            const double a = m[0] / scale, b = m[1] / scale, c = m[2] / scale, d = m[3] / scale;
            const double determinant = std::abs(a * d - b * c);
            // Frobenius^2 / |det| = condition + 1/condition. Both the
            // user-space and device-space matrices must be safe for FT16.16.
            if (determinant == 0 || (a * a + b * b + c * c + d * d) / determinant > 4096.000244140625)
                fail(Error::UnsupportedInput, "Text transform anisotropy exceeds the supported outline precision range.");
        };
        checkCondition(local);
        checkCondition(device);
        if (std::abs(device[0] * device[3] - device[1] * device[2]) < 0.01)
            fail(Error::UnsupportedInput, "Near-singular text transform would trigger a substituted Splash font matrix.");
        // SplashFTFont chooses its FT pixel size from the vertical device basis,
        // rounds to an integer (minimum one), then transforms 26.6 coordinates.
        // A fixed post-transform 0.1bp guard is insufficient under anisotropy.
        const double pixels = std::max(1.0, std::floor(std::hypot(device[2], device[3]) + 0.5));
        const double a = inverse[0] * device[0] + inverse[2] * device[1];
        const double b = inverse[1] * device[0] + inverse[3] * device[1];
        const double c = inverse[0] * device[2] + inverse[2] * device[3];
        const double d = inverse[1] * device[2] + inverse[3] * device[3];
        // 0.1 FT pixels exceeds several 26.6 quantization steps; propagate its
        // square envelope through each matrix row into original PDF coordinates.
        return { 0.1 * std::max(1.0, (std::abs(a) + std::abs(c)) / pixels), 0.1 * std::max(1.0, (std::abs(b) + std::abs(d)) / pixels) };
    }
    Box pathBox(GfxState *state, bool stroke)
    {
        Box box;
        bool joins = false;
        const auto *path = state->getPath();
        for (int i = 0; i < path->getNumSubpaths(); ++i) {
            const auto *p = path->getSubpath(i);
            joins = joins || p->getNumPoints() > 2;
            for (int j = 0; j < p->getNumPoints(); ++j)
                point(box, state, p->getX(j), p->getY(j));
        }
        // Cubic curves lie in the hull of their control points: no flattening error.
        if (stroke)
            widenStroke(box, state, joins);
        return box;
    }
    void image(GfxState *state, int width, int height)
    {
        if (width <= 0 || height <= 0)
            fail(Error::DamagedInput, "Invalid image dimensions.");
        Box b;
        point(b, state, 0, 0);
        point(b, state, 0, 1);
        point(b, state, 1, 0);
        point(b, state, 1, 1);
        commit(b);
    }
    void consumeInline(Stream *stream, int width, int height, int comps, int bits)
    {
        if (width <= 0 || height <= 0 || comps <= 0 || bits <= 0 || bits > 16)
            fail(Error::DamagedInput, "Invalid inline image.");
        if (comps > 32)
            fail(Error::DamagedInput, "Invalid inline image component count.");
        const uint64_t row = (uint64_t(width) * comps * bits + 7) / 8;
        if (row > byteLimit || uint64_t(height) > byteLimit / std::max(uint64_t(1), row))
            fail(Error::LimitExceeded, "Decoded inline image budget exceeded.");
        const uint64_t total = row * uint64_t(height);
        if (total > byteLimit || budget.bytes + total > byteLimit)
            fail(Error::LimitExceeded, "Decoded inline image budget exceeded.");
        budget.bytes += static_cast<size_t>(total);
        if (!stream->rewind())
            fail(Error::DamagedInput, "Cannot read inline image.");
        for (uint64_t i = 0; i < total; ++i) {
            if ((i & 4095) == 0)
                budget.check();
            if (stream->getChar() == EOF)
                fail(Error::DamagedInput, "Truncated inline image.");
        }
        stream->close();
    }

public:
    InkDevice(PDFDoc *doc, Budget &b) : budget(b) { fonts.startDoc(doc); }
    bool upsideDown() override { return false; }
    bool useDrawChar() override { return true; }
    bool interpretType3Chars() override { return true; }
    void setDefaultCTM(const std::array<double, 6> &m) override
    {
        OutputDev::setDefaultCTM(m);
        for (double n : m)
            finite(n);
        double d = m[0] * m[3] - m[1] * m[2];
        if (!std::isfinite(d) || std::abs(d) < 1e-20)
            fail(Error::UnsupportedInput, "Singular inspection transform.");
        inverse = { m[3] / d, -m[1] / d, -m[2] / d, m[0] / d, (m[2] * m[5] - m[3] * m[4]) / d, (m[1] * m[4] - m[0] * m[5]) / d };
    }
    void startPage(int n, GfxState *state, XRef *xref) override
    {
        fonts.startPage(n, nullptr, xref);
        fonts.updateCTM(state, 0, 0, 0, 0, 0, 0);
    }
    void saveState(GfxState *) override
    {
        budget.point();
        if (clips.size() > 512)
            fail(Error::LimitExceeded, "Graphics state nesting limit exceeded.");
        clips.push_back(clipBox);
    }
    void restoreState(GfxState *) override
    {
        if (clips.empty())
            fail(Error::DamagedInput, "Unbalanced graphics state.");
        clipBox = clips.back();
        clips.pop_back();
    }
    void clip(GfxState *s) override
    {
        Box b = pathBox(s, false);
        clipBox = b.any ? intersect(clipBox, b.rect) : Rect { };
    }
    void eoClip(GfxState *s) override { clip(s); }
    void clipToStrokePath(GfxState *s) override
    {
        Box b = pathBox(s, true);
        // Core invokes this for pattern-painted strokes. Record their conservative
        // envelope before Core's own clipToStrokePath approximation drops miters.
        commit(b);
        clipBox = b.any ? intersect(clipBox, b.rect) : Rect { };
    }
    void stroke(GfxState *s) override
    {
        if (!s->getStrokeColorSpace()->isNonMarking())
            commit(pathBox(s, true));
    }
    void fill(GfxState *s) override
    {
        if (!s->getFillColorSpace()->isNonMarking())
            commit(pathBox(s, false));
    }
    void eoFill(GfxState *s) override { fill(s); }
    void drawChar(GfxState *s, double x, double y, double, double, double ox, double oy, CharCode code, int, const Unicode *unicode, int unicodeLength) override
    {
        budget.point();
        const int render = s->getRender();
        if (render == 3 || render == 7)
            return;
        const bool fill = !(render & 1) && !s->getFillColorSpace()->isNonMarking();
        const bool stroke = ((render & 3) == 1 || (render & 3) == 2) && !s->getStrokeColorSpace()->isNonMarking();
        if (!fill && !stroke)
            return;
        const auto &gfxFont = s->getFont();
        Ref embedded;
        if (gfxFont && !gfxFont->isCIDFont() && !gfxFont->isSymbolic() && !gfxFont->getEmbeddedFontID(&embedded) && unicodeLength == 1 && unicode[0] >= 0x80 && unicode[0] <= 0xffff)
            fail(Error::UnsupportedInput, "Unembedded Unicode fallback text has no stable source-font outline.");
        const auto precision = glyphPrecision(s);
        SplashFont *font = fonts.prepare(s);
        if (!font)
            fail(Error::UnsupportedInput, "Cannot obtain a physical font outline for appearance text.");
        std::unique_ptr<SplashPath> path(font->getGlyphPath(code));
        if (!path)
            fail(Error::UnsupportedInput, "Cannot obtain a glyph outline; no font-wide bbox fallback is used.");
        Box b;
        for (int i = 0; i < path->getLength(); ++i) {
            double a, c;
            unsigned char flags;
            path->getPoint(i, &a, &c, &flags);
            point(b, s, a + x - ox, c + y - oy);
        }
        if (b.any) {
            // FreeType outline coordinates have finite fixed-point precision.
            // This small geometric guard is not a font ascent/descent rectangle.
            b.rect.x1 -= precision[0];
            b.rect.y1 -= precision[1];
            b.rect.x2 += precision[0];
            b.rect.y2 += precision[1];
            if (stroke)
                widenStroke(b, s, true);
            commit(b);
        }
        // Text clipping is conservatively ignored (never narrows later marks).
    }
    bool beginType3Char(GfxState *, double, double, double, double, CharCode, const Unicode *, int) override
    {
        budget.point();
        return false;
    }
    void drawImage(GfxState *s, Object *, Stream *str, int w, int h, GfxImageColorMap *map, bool, const int *, bool inl) override
    {
        image(s, w, h);
        if (inl)
            consumeInline(str, w, h, map->getNumPixelComps(), map->getBits());
    }
    void drawImageMask(GfxState *s, Object *, Stream *str, int w, int h, bool, bool, bool inl) override
    {
        image(s, w, h);
        if (inl)
            consumeInline(str, w, h, 1, 1);
    }
    void drawMaskedImage(GfxState *s, Object *, Stream *, int w, int h, GfxImageColorMap *, bool, Stream *, int, int, bool, bool) override { image(s, w, h); }
    void drawSoftMaskedImage(GfxState *s, Object *, Stream *, int w, int h, GfxImageColorMap *, bool, Stream *, int, int, GfxImageColorMap *, bool) override { image(s, w, h); }
    void beginTransparencyGroup(GfxState *, const std::array<double, 4> &, GfxColorSpace *, bool, bool, bool forMask) override
    {
        groups.push_back(forMask);
        if (forMask)
            ++softMaskDepth;
    }
    void endTransparencyGroup(GfxState *) override
    {
        if (groups.empty())
            fail(Error::DamagedInput, "Unbalanced transparency group.");
        if (groups.back())
            --softMaskDepth;
        groups.pop_back();
    }
    void psXObject(Stream *, Stream *) override { fail(Error::UnsupportedInput, "PostScript XObjects cannot be measured safely."); }
    bool hasInk() const { return marked; }
    Rect bounds() const { return ink; }
};

void scanObject(const Object &obj, int depth, Budget &budget)
{
    if (++budget.objects > 2000000 || depth > 128)
        fail(Error::LimitExceeded, "PDF object graph inspection limit exceeded.");
    if ((budget.objects & 255) == 0)
        budget.check();
    if (obj.isRef())
        return;
    if (obj.isNum())
        finite(obj.getNum());
    if (obj.isArray()) {
        for (int i = 0; i < obj.arrayGetLength(); ++i)
            scanObject(obj.arrayGetNF(i), depth + 1, budget);
    } else if (obj.isDict() || obj.isStream()) {
        Dict *d = obj.isDict() ? obj.getDict() : obj.streamGetDict();
        if (d->hasKey("ByteRange") || d->lookup("Type").isName("Sig") || d->lookup("Type").isName("DocTimeStamp") || (d->lookup("FT").isName("Sig") && !d->lookup("V").isNull()))
            fail(Error::UnsupportedInput, "Signed PDFs cannot be rewritten as appearances.");
        if (d->lookup("Subtype").isName("PS"))
            fail(Error::UnsupportedInput, "PostScript XObjects are unsupported.");
        if (d->hasKey("OPI") || d->hasKey("Ref"))
            fail(Error::UnsupportedInput, "External/OPI appearance content is unsupported.");
        if (obj.isStream()) {
            Object f = d->lookup("Filter");
            const std::set<std::string> filters = {
                "FlateDecode", "Fl", "LZWDecode", "LZW", "ASCII85Decode", "A85", "ASCIIHexDecode", "AHx", "RunLengthDecode", "RL", "DCTDecode", "DCT", "JPXDecode", "JBIG2Decode", "CCITTFaxDecode", "CCF"
            };
            auto checkFilter = [&](const Object &n) {
                if (!n.isName() || !filters.count(n.getName()))
                    fail(Error::UnsupportedInput, "Unsupported stream filter in appearance PDF.");
            };
            if (f.isArray()) {
                for (int i = 0; i < f.arrayGetLength(); ++i) {
                    Object n = f.arrayGet(i);
                    checkFilter(n);
                }
            } else if (!f.isNull())
                checkFilter(f);
            // Bound decoded font/content/CMap data before Gfx/font loading. Image
            // pixels are not needed: their complete transformed rectangle is used.
            if (!d->lookup("Subtype").isName("Image")) {
                Stream *s = obj.getStream();
                if (!s->rewind())
                    fail(Error::DamagedInput, "Cannot read appearance stream.");
                while (s->getChar() != EOF) {
                    if (++budget.bytes > byteLimit)
                        fail(Error::LimitExceeded, "Decoded stream budget exceeds 64 MiB.");
                    if ((budget.bytes & 4095) == 0)
                        budget.check();
                }
                s->close();
            }
        }
        for (int i = 0; i < d->getLength(); ++i)
            scanObject(d->getValNF(i), depth + 1, budget);
    }
}
Object inheritedEntry(const Object &page, const char *key, XRef *xref)
{
    Object current = page.copy();
    std::set<std::pair<int, int>> seen;
    for (int depth = 0; depth < 128; ++depth) {
        if (!current.isDict())
            fail(Error::DamagedInput, "Invalid page inheritance chain.");
        Object found = current.dictLookup(key);
        if (!found.isNull())
            return found;
        const Object &parent = current.dictLookupNF("Parent");
        if (parent.isNull())
            return Object::null();
        if (parent.isRef() && !seen.insert({ parent.getRefNum(), parent.getRefGen() }).second)
            fail(Error::DamagedInput, "Cyclic page inheritance.");
        current = parent.fetch(xref);
    }
    fail(Error::LimitExceeded, "Page inheritance nesting limit exceeded.");
}
std::string number(double value)
{
    finite(value);
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << std::fixed << std::setprecision(12) << value;
    return o.str();
}
Object rectangle(XRef *xref, const Rect &r)
{
    Object a(new Array(xref));
    for (double n : { r.x1, r.y1, r.x2, r.y2 })
        a.arrayAdd(Object(n));
    return a;
}
Ref stream(XRef *xref, const std::string &s)
{
    return xref->addStreamObject(new Dict(xref), std::vector<char>(s.begin(), s.end()), StreamCompression::Compress);
}
void appendContents(Object &array, const Object &original, XRef *xref, int depth = 0)
{
    if (depth > 8)
        fail(Error::DamagedInput, "Nested Contents exceeds limits.");
    Object fetched = original.fetch(xref);
    if (fetched.isNull())
        return;
    if (fetched.isArray()) {
        for (int i = 0; i < fetched.arrayGetLength(); ++i)
            appendContents(array, fetched.arrayGetNF(i), xref, depth + 1);
    } else if (fetched.isStream())
        array.arrayAdd(original.isRef() ? original.copy() : Object(xref->addIndirectObject(fetched)));
    else
        fail(Error::DamagedInput, "Page Contents must contain streams.");
}
struct TemporaryDirectory
{
    fs::path path;
    ~TemporaryDirectory()
    {
        if (!path.empty()) {
            std::error_code e;
            fs::remove_all(path, e);
        }
    }
};
std::string utf8(const fs::path &p)
{
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
} // namespace

namespace PdfPageBounds {
Result expandAppearance(const std::string &utf8Input, const std::string &utf8Output, double padding)
{
    Result result;
    try {
        Budget budget;
        if (utf8Input.empty() || utf8Output.empty() || utf8Input.find('\0') != std::string::npos || utf8Output.find('\0') != std::string::npos || !std::isfinite(padding) || padding < 0 || padding > 1000)
            fail(Error::InvalidArguments, "Invalid paths or padding.");
        const fs::path input = fs::u8path(utf8Input), output = fs::u8path(utf8Output);
        std::error_code ec;
        auto status = fs::symlink_status(output, ec);
        if (fs::exists(status))
            fail(Error::InvalidArguments, "Output must be a new file; existing files and aliases are never overwritten.");
        if (ec && ec != std::errc::no_such_file_or_directory)
            fail(Error::IoError, "Cannot inspect output: " + ec.message());
        if (fs::weakly_canonical(input) == fs::weakly_canonical(output))
            fail(Error::InvalidArguments, "Input and output paths must differ.");
        if (!fs::is_regular_file(input) || fs::file_size(input) > byteLimit)
            fail(Error::LimitExceeded, "Input must be a regular PDF of at most 64 MiB.");
        // Respect Poppler's reference-counted global initialization and its lock.
        GlobalParamsIniter initer(nullptr);
        PDFDoc doc(std::make_unique<GooString>(utf8Input));
        if (!doc.isOk())
            fail(doc.getErrorCode() == errEncrypted ? Error::UnsupportedInput : Error::DamagedInput, "Cannot open appearance PDF (encrypted or damaged input).");
        if (doc.isEncrypted())
            fail(Error::UnsupportedInput, "Encrypted appearance PDFs are unsupported.");
        if (doc.getNumPages() != 1)
            fail(Error::UnsupportedInput, "Appearance must have exactly one page.");
        XRef *xref = doc.getXRef();
        for (int i = 0; i < xref->getNumObjects(); ++i) {
            budget.check();
            const XRefEntry *e = xref->getEntry(i);
            if (!e || e->type == xrefEntryFree || e->type == xrefEntryNone)
                continue;
            Object o = xref->fetch(Ref { i, e->type == xrefEntryCompressed ? 0 : e->gen });
            scanObject(o, 0, budget);
        }
        Page *page = doc.getPage(1);
        if (!page || !page->isOk())
            fail(Error::DamagedInput, "Invalid appearance page.");
        if (page->getRotate() != 0)
            fail(Error::UnsupportedInput, "Rotated appearance pages are unsupported.");
        Object pageObject = xref->fetch(page->getRef());
        if (!pageObject.isDict())
            fail(Error::DamagedInput, "Invalid page dictionary.");
        Object unit = inheritedEntry(pageObject, "UserUnit", xref);
        if (!unit.isNull() && (!unit.isNum() || unit.getNum() != 1))
            fail(Error::UnsupportedInput, "Appearance UserUnit must be one.");
        Object rotate = inheritedEntry(pageObject, "Rotate", xref);
        if (!rotate.isNull() && (!rotate.isInt() || rotate.getInt() != 0))
            fail(Error::UnsupportedInput, "Appearance Rotate must be zero.");
        Object media = inheritedEntry(pageObject, "MediaBox", xref);
        if (!media.isArray() || media.arrayGetLength() != 4)
            fail(Error::DamagedInput, "Appearance MediaBox must contain four numbers.");
        double dimensions[4];
        for (int i = 0; i < 4; ++i) {
            Object v = media.arrayGet(i);
            if (!v.isNum())
                fail(Error::DamagedInput, "Appearance MediaBox must be numeric.");
            dimensions[i] = v.getNum();
        }
        result.logicalBounds = { dimensions[0], dimensions[1], dimensions[2], dimensions[3] };
        const Rect &logical = result.logicalBounds;
        for (double n : { logical.x1, logical.y1, logical.x2, logical.y2 }) {
            finite(n);
            if (std::abs(n) >= inspectionLimit - 2)
                fail(Error::LimitExceeded, "Logical page is outside the inspection area.");
        }
        if (!positive(logical) || logical.x2 - logical.x1 > spanLimit || logical.y2 - logical.y1 > spanLimit)
            fail(Error::UnsupportedInput, "Invalid or excessive logical page dimensions.");
        InkDevice device(&doc, budget);
        {
            PDFRectangle inspection(-inspectionLimit, -inspectionLimit, inspectionLimit, inspectionLimit);
            Gfx gfx(&doc, &device, 1, page->getResourceDict(), 72, 72, &inspection, nullptr, 0, Budget::callback, &budget);
            Object contents = page->getContents();
            if (!contents.isNull())
                gfx.display(&contents);
        }
        budget.check();
        result.hasInk = device.hasInk();
        result.inkBounds = device.bounds();
        result.outputBounds = logical;
        if (result.hasInk) {
            Rect ink = result.inkBounds;
            ink.x1 -= padding;
            ink.y1 -= padding;
            ink.x2 += padding;
            ink.y2 += padding;
            result.outputBounds = unite(logical, ink);
        }
        Rect &bounds = result.outputBounds;
        // Outward rounding, never round a glyph edge inward.
        bounds.x1 = std::floor(bounds.x1 * 1000000) / 1000000;
        bounds.y1 = std::floor(bounds.y1 * 1000000) / 1000000;
        bounds.x2 = std::ceil(bounds.x2 * 1000000) / 1000000;
        bounds.y2 = std::ceil(bounds.y2 * 1000000) / 1000000;
        double width = bounds.x2 - bounds.x1, height = bounds.y2 - bounds.y1;
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 || width > spanLimit || height > spanLimit)
            fail(Error::LimitExceeded, "Expanded page exceeds the 50000bp span limit.");
        Object contentArray(new Array(xref));
        contentArray.arrayAdd(Object(stream(xref, "q\n1 0 0 1 " + number(-bounds.x1) + " " + number(-bounds.y1) + " cm\n")));
        appendContents(contentArray, pageObject.dictLookupNF("Contents"), xref);
        contentArray.arrayAdd(Object(stream(xref, "\nQ\n")));
        pageObject.dictSet("Contents", std::move(contentArray));
        const Rect normalized { 0, 0, width, height };
        for (const char *key : { "MediaBox", "CropBox", "TrimBox", "BleedBox", "ArtBox" })
            pageObject.dictSet(key, rectangle(xref, normalized));
        pageObject.getDict()->remove("Annots");
        pageObject.getDict()->remove("AA");
        xref->setModifiedObject(&pageObject, page->getRef());
        Ref root = xref->getRoot();
        Object catalog = xref->fetch(root);
        if (!catalog.isDict())
            fail(Error::DamagedInput, "Invalid catalog.");
        catalog.getDict()->remove("AcroForm");
        catalog.getDict()->remove("OpenAction");
        catalog.getDict()->remove("AA");
        xref->setModifiedObject(&catalog, root);
        TemporaryDirectory temporary;
        std::random_device random;
        for (int attempt = 0; attempt < 32 && temporary.path.empty(); ++attempt) {
            fs::path candidate = output;
            candidate += ".bounds-" + std::to_string(random()) + ".tmp";
            ec.clear();
            if (fs::create_directory(candidate, ec))
                temporary.path = candidate;
            else if (ec && ec != std::errc::file_exists)
                fail(Error::IoError, "Cannot reserve output temporary directory: " + ec.message());
        }
        if (temporary.path.empty())
            fail(Error::IoError, "Cannot reserve output temporary directory.");
        const fs::path temporaryFile = temporary.path / "output.pdf";
        if (doc.saveAs(utf8(temporaryFile), writeForceRewrite) != errNone)
            fail(Error::WriteError, "Could not write expanded appearance PDF.");
        if (fs::file_size(temporaryFile) > byteLimit)
            fail(Error::LimitExceeded, "Expanded PDF exceeds the 64 MiB limit.");
        {
            PDFDoc verify(std::make_unique<GooString>(utf8(temporaryFile)));
            if (!verify.isOk() || verify.getNumPages() != 1 || !verify.getPage(1) || !verify.getPage(1)->isOk())
                fail(Error::WriteError, "Expanded PDF failed reopen validation.");
            const PDFRectangle *box = verify.getPage(1)->getMediaBox();
            if (box->x1 != 0 || box->y1 != 0 || std::abs(box->x2 - width) > 1e-5 || std::abs(box->y2 - height) > 1e-5)
                fail(Error::WriteError, "Expanded page box failed validation.");
        }
        budget.check();
        ec.clear();
        fs::create_hard_link(temporaryFile, output, ec);
        if (ec)
            fail(Error::IoError, "Cannot publish without replacing a file (hard-link support required): " + ec.message());
        result.ok = true;
    } catch (const Failure &f) {
        result.error = f.error;
        result.message = f.message;
    } catch (const std::exception &e) {
        result.error = Error::IoError;
        result.message = e.what();
    }
    return result;
}
} // namespace PdfPageBounds
