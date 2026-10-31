#include "subtitles.h"

#include "playback.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <string_view>

#include <windows.h>

namespace
{
std::wstring toWide(const std::string& text, UINT codePage, DWORD flags)
{
    const int size = MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string toUtf8(const std::wstring& text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>((std::max)(size, 0)), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    }
    return out;
}

std::wstring trim(std::wstring_view text)
{
    const auto first = text.find_first_not_of(L" \t\x00A0");
    if (first == std::wstring_view::npos) {
        return {};
    }
    return std::wstring(text.substr(first, text.find_last_not_of(L" \t\x00A0") - first + 1));
}

std::wstring lower(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

// "01:02:03,456", "01:02:03.456" or "02:03.456" -> seconds
bool parseTime(const std::wstring& text, double& seconds)
{
    std::vector<std::wstring> parts;
    std::size_t start = 0;
    for (;;) {
        const auto colon = text.find(L':', start);
        parts.push_back(text.substr(start, colon == std::wstring::npos ? std::wstring::npos : colon - start));
        if (colon == std::wstring::npos) {
            break;
        }
        start = colon + 1;
    }
    if (parts.size() < 2 || parts.size() > 3) {
        return false;
    }
    try {
        std::wstring last = parts.back();
        std::replace(last.begin(), last.end(), L',', L'.');
        const double h = parts.size() == 3 ? std::stod(parts[0]) : 0.0;
        const double m = std::stod(parts[parts.size() - 2]);
        const double s = std::stod(last);
        seconds = h * 3600.0 + m * 60.0 + s;
        return seconds >= 0;
    } catch (...) {
        return false;
    }
}

// Tags and entities out: <i>, </i>, <c>, <00:00:01.000>, <font ...>, {\an8}, &amp; ...
std::wstring cleanLine(const std::wstring& line)
{
    std::wstring out;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == L'<') {
            const auto close = line.find(L'>', i);
            if (close != std::wstring::npos) {
                i = close;
                continue;
            }
        } else if (line[i] == L'{' && i + 1 < line.size() && line[i + 1] == L'\\') {
            const auto close = line.find(L'}', i);
            if (close != std::wstring::npos) {
                i = close;
                continue;
            }
        }
        out.push_back(line[i]);
    }
    static const std::pair<const wchar_t*, const wchar_t*> entities[] = {
        { L"&nbsp;", L" " }, { L"&lt;", L"<" }, { L"&gt;", L">" }, { L"&quot;", L"\"" }, { L"&#39;", L"'" },
        { L"&apos;", L"'" }, { L"&lrm;", L"" }, { L"&rlm;", L"" }, { L"&amp;", L"&" }
    };
    for (const auto& [from, to] : entities) {
        for (std::size_t at = out.find(from); at != std::wstring::npos; at = out.find(from, at + wcslen(to))) {
            out.replace(at, wcslen(from), to);
        }
    }
    return trim(out);
}
}

namespace Subtitles
{
Cues parse(const std::string& raw)
{
    std::string bytes = raw;
    if (bytes.starts_with("\xEF\xBB\xBF")) {
        bytes.erase(0, 3);
    }
    // UTF-8 when valid; older .srt files are often in the Windows code page (Spanish accents in Windows-1252)
    std::wstring text = toWide(bytes, CP_UTF8, MB_ERR_INVALID_CHARS);
    if (text.empty() && !bytes.empty()) {
        text = toWide(bytes, CP_ACP, 0);
    }
    std::erase(text, L'\r');

    Cues cues;
    Cue cue;
    bool inCue = false;
    std::vector<std::wstring> lines;
    auto finish = [&] {
        if (inCue) {
            std::wstring joined;
            for (const auto& l : lines) {
                const std::wstring c = cleanLine(l);
                if (!c.empty()) {
                    joined += (joined.empty() ? L"" : L"\n") + c;
                }
            }
            if (!joined.empty() && cue.end - cue.start >= 0.05) {
                cue.text = std::move(joined);
                cues.push_back(std::move(cue));
            }
        }
        inCue = false;
        lines.clear();
        cue = {};
    };
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find(L'\n', start);
        if (end == std::wstring::npos) {
            end = text.size();
        }
        const std::wstring line = text.substr(start, end - start);
        start = end + 1;
        const auto arrow = line.find(L"-->");
        if (arrow != std::wstring::npos) {
            finish();
            const std::wstring left = trim(line.substr(0, arrow));
            std::wstring right = trim(line.substr(arrow + 3));
            right = right.substr(0, right.find_first_of(L" \t"));   // VTT cue settings follow the end time
            inCue = parseTime(left, cue.start) && parseTime(right, cue.end);
            continue;
        }
        if (!inCue) {
            continue;   // SRT numbers, WEBVTT header, cue identifiers, NOTE/STYLE blocks
        }
        if (trim(line).empty()) {
            if (lines.empty()) {
                continue;   // YouTube: a blank-looking first line before the text
            }
            finish();
            continue;
        }
        lines.push_back(line);
    }
    finish();

    std::stable_sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
    Cues merged;
    for (auto& c : cues) {
        if (!merged.empty() && merged.back().text == c.text && c.start <= merged.back().end + 0.05) {
            merged.back().end = (std::max)(merged.back().end, c.end);
        } else {
            merged.push_back(std::move(c));
        }
    }
    for (std::size_t i = 0; i + 1 < merged.size(); ++i) {
        const double gap = merged[i + 1].start - merged[i].end;
        if (gap > 0 && gap < 0.25) {
            merged[i].end = merged[i + 1].start;
        }
    }
    return merged;
}

Cues load(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    std::string bytes(4 * 1024 * 1024 + 1, '\0');   // a subtitle file is far smaller
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(in.gcount()));
    if (bytes.size() > 4 * 1024 * 1024) {
        return {};
    }
    return parse(bytes);
}

std::vector<Track> find(const std::filesystem::path& dir, const std::wstring& stem)
{
    std::vector<Track> tracks;
    const std::wstring wanted = lower(stem);
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const auto ext = lower(it->path().extension().wstring());
        if (ext != L".srt" && ext != L".vtt") {
            continue;
        }
        const std::wstring base = it->path().stem().wstring();
        const std::wstring baseLower = lower(base);
        if (baseLower == wanted) {
            tracks.push_back({ "", it->path() });
        } else if (baseLower.size() > wanted.size() + 1 && baseLower.starts_with(wanted + L".")) {
            const std::wstring tag = base.substr(wanted.size() + 1);
            if (tag.size() <= 16 && std::all_of(tag.begin(), tag.end(), [](wchar_t c) { return std::iswalnum(c) || c == L'-' || c == L'_'; })) {
                tracks.push_back({ toUtf8(tag), it->path() });
            }
        }
    }
    std::sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) {
        return a.language != b.language ? a.language < b.language : a.file < b.file;
    });
    return tracks;
}

bool sameLanguage(const std::string& tag, const std::string& wanted)
{
    auto base = [](const std::string& t) {
        std::string b = t.substr(0, t.find_first_of("-_"));
        std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return b;
    };
    return !tag.empty() && !wanted.empty() && base(tag) == base(wanted);
}

const Track* pick(const std::vector<Track>& tracks, const std::vector<std::string>& preferred, const std::string& choice,
                  bool allowUntagged)
{
    if (choice == "off") {
        return nullptr;
    }
    if (!choice.empty()) {
        for (const auto& t : tracks) {
            if (t.language == choice || (choice == "-" && t.language.empty())) {
                return &t;
            }
        }
    }
    for (const auto& want : preferred) {
        const Track* found = nullptr;
        for (const auto& t : tracks) {
            if (sameLanguage(t.language, want) && (!found || t.language.size() < found->language.size())) {
                found = &t;   // "es" before "es-419" before "es-orig"
            }
        }
        if (found) {
            return found;
        }
    }
    if (allowUntagged) {
        for (const auto& t : tracks) {
            if (t.language.empty()) {
                return &t;
            }
        }
    }
    return nullptr;
}

std::string label(const std::string& language)
{
    if (language.empty()) {
        return "ON";
    }
    std::string out = language.substr(0, language.find_first_of("-_"));
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (language.size() > 5 && language.ends_with("-orig")) {
        out += " (AUTO)";
    }
    return out;
}

Image render(const std::wstring& text, std::uint32_t maxWidth, int pixelHeight)
{
    Image image;
    if (text.empty() || maxWidth < 16 || pixelHeight < 6) {
        return image;
    }
    std::wstring lines;   // DrawText breaks lines at CR LF
    for (wchar_t c : text) {
        if (c == L'\n') {
            lines += L"\r\n";
        } else {
            lines.push_back(c);
        }
    }
    const int radius = (std::max)(2, pixelHeight / 12);
    const int pad = radius + 1;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        return image;
    }
    HFONT font = CreateFontW(-pixelHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    const UINT format = DT_CENTER | DT_WORDBREAK | DT_NOPREFIX;
    RECT measure{ 0, 0, static_cast<LONG>(maxWidth) - 2 * pad, 0 };
    DrawTextW(dc, lines.c_str(), -1, &measure, format | DT_CALCRECT);
    const int w = (std::min)(static_cast<int>(maxWidth), static_cast<int>(measure.right) + 2 * pad);
    const int h = static_cast<int>(measure.bottom) + 2 * pad;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = w > 0 && h > 0 ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (bitmap && bits) {
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        std::memset(bits, 0, static_cast<std::size_t>(w) * h * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT box{ pad, pad, w - pad, h - pad };
        DrawTextW(dc, lines.c_str(), -1, &box, format);
        GdiFlush();
        image.width = static_cast<std::uint32_t>(w);
        image.height = static_cast<std::uint32_t>(h);
        image.fill.resize(static_cast<std::size_t>(w) * h);
        const auto* px = static_cast<const std::uint8_t*>(bits);
        for (std::size_t i = 0; i < image.fill.size(); ++i) {
            image.fill[i] = (std::max)({ px[i * 4], px[i * 4 + 1], px[i * 4 + 2] });   // grey anti-aliasing: any channel
        }
        // outline: the letters grown by `radius` (separable max filter)
        std::vector<std::uint8_t> row(image.fill.size());
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                std::uint8_t m = 0;
                for (int k = (std::max)(0, x - radius); k <= (std::min)(w - 1, x + radius); ++k) {
                    m = (std::max)(m, image.fill[static_cast<std::size_t>(y) * w + k]);
                }
                row[static_cast<std::size_t>(y) * w + x] = m;
            }
        }
        image.edge.resize(image.fill.size());
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                std::uint8_t m = 0;
                for (int k = (std::max)(0, y - radius); k <= (std::min)(h - 1, y + radius); ++k) {
                    m = (std::max)(m, row[static_cast<std::size_t>(k) * w + x]);
                }
                image.edge[static_cast<std::size_t>(y) * w + x] = m;
            }
        }
        SelectObject(dc, oldBitmap);
    }
    if (bitmap) {
        DeleteObject(bitmap);
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    return image;
}

void blend(std::uint8_t* frame, std::uint32_t pitch, std::uint32_t width, std::uint32_t height, const Image& image,
           std::uint32_t bottomMargin)
{
    if (image.width == 0 || image.height == 0) {
        return;
    }
    const int x0 = (static_cast<int>(width) - static_cast<int>(image.width)) / 2;
    const int y0 = static_cast<int>(height) - static_cast<int>(bottomMargin) - static_cast<int>(image.height);
    for (std::uint32_t iy = 0; iy < image.height; ++iy) {
        const int y = y0 + static_cast<int>(iy);
        if (y < 0 || y >= static_cast<int>(height)) {
            continue;
        }
        std::uint8_t* line = frame + static_cast<std::size_t>(y) * pitch;
        for (std::uint32_t ix = 0; ix < image.width; ++ix) {
            const int x = x0 + static_cast<int>(ix);
            const std::size_t i = static_cast<std::size_t>(iy) * image.width + ix;
            const unsigned e = image.edge[i];
            const unsigned f = image.fill[i];
            if ((e | f) == 0 || x < 0 || x >= static_cast<int>(width)) {
                continue;
            }
            std::uint8_t* p = line + static_cast<std::size_t>(x) * 4;
            const unsigned keep = 255 - e * 205 / 255;   // the outline darkens to 20%
            for (int c = 0; c < 3; ++c) {
                unsigned v = p[c] * keep / 255;
                v += (255 - v) * f / 255;
                p[c] = static_cast<std::uint8_t>(v);
            }
        }
    }
}
}

SubtitledSource::SubtitledSource(std::shared_ptr<Playback> video, std::uint32_t width, std::uint32_t height, float scale) :
    video_(std::move(video)), width_(width), height_(height),
    pixelHeight_((std::max)(12, static_cast<int>(height * 0.06f * std::clamp(scale, 0.5f, 2.5f) + 0.5f)))
{}

SubtitledSource::~SubtitledSource()
{
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void SubtitledSource::setCues(std::shared_ptr<const Subtitles::Cues> cues)
{
    std::lock_guard lock(mutex_);
    cues_ = cues && !cues->empty() ? std::move(cues) : nullptr;
    ++version_;
    wanted_ = -1;
    rendered_.clear();
    if (cues_ && !thread_.joinable()) {
        thread_ = std::thread([this] { loop(); });
    }
    cv_.notify_all();
}

int SubtitledSource::cueAtLocked(double seconds) const
{
    if (!cues_) {
        return -1;
    }
    const auto& cues = *cues_;
    const auto it = std::upper_bound(cues.begin(), cues.end(), seconds, [](double s, const Subtitles::Cue& c) { return s < c.start; });
    if (it == cues.begin()) {
        return -1;
    }
    const auto index = static_cast<int>(std::distance(cues.begin(), it)) - 1;
    return seconds < cues[static_cast<std::size_t>(index)].end ? index : -1;
}

std::uint64_t SubtitledSource::copyLatest(std::uint64_t lastSeq, std::uint8_t* dst, std::uint32_t dstPitch)
{
    const double position = video_->positionSeconds();
    std::shared_ptr<const Subtitles::Image> image;
    std::uint64_t version = 0;
    int index = -1;
    {
        std::lock_guard lock(mutex_);
        version = version_;
        index = cueAtLocked(position);
        if (index >= 0) {
            for (const auto& r : rendered_) {
                if (r.version == version && r.index == index) {
                    image = r.image;
                }
            }
            if (!image && wanted_ != index) {
                wanted_ = index;
                cv_.notify_all();
            }
        }
    }
    const int shown = image ? index : -1;
    const bool force = lastSeq == 0 || lastSeq != outSeq_ || shown != drawnIndex_ || (shown >= 0 && version != drawnVersion_);
    const std::uint64_t seq = video_->copyLatest(force ? 0 : innerSeq_, dst, dstPitch);
    if (seq == 0) {
        return 0;
    }
    if (image) {
        Subtitles::blend(dst, dstPitch, width_, height_, *image, height_ / 20);
    }
    innerSeq_ = seq;
    drawnIndex_ = shown;
    drawnVersion_ = version;
    return ++outSeq_;
}

void SubtitledSource::onSurfaceLive(bool live)
{
    video_->onSurfaceLive(live);
}

void SubtitledSource::loop()
{
    std::unique_lock lock(mutex_);
    while (!quit_) {
        int todo = -1;
        if (cues_ && wanted_ >= 0) {
            for (int i : { wanted_, wanted_ + 1 }) {
                const bool done = std::any_of(rendered_.begin(), rendered_.end(),
                                              [&](const Rendered& r) { return r.version == version_ && r.index == i; });
                if (i < static_cast<int>(cues_->size()) && !done) {
                    todo = i;
                    break;
                }
            }
        }
        if (todo < 0) {
            cv_.wait(lock);
            continue;
        }
        const auto cues = cues_;
        const auto version = version_;
        lock.unlock();
        auto image = std::make_shared<const Subtitles::Image>(
            Subtitles::render((*cues)[static_cast<std::size_t>(todo)].text, width_ * 9 / 10, pixelHeight_));
        lock.lock();
        if (version != version_) {
            continue;
        }
        std::erase_if(rendered_, [&](const Rendered& r) {
            return r.version != version_ || (r.index != wanted_ && r.index != wanted_ + 1);
        });
        rendered_.push_back({ version, todo, std::move(image) });
    }
}
