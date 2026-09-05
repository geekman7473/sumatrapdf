/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "DocController.h"
#include "gui/UIModels.h"
#include "EngineBase.h"
#include "ProgressUpdateUI.h"
#include "TextSelection.h"
#include "TextSearch.h"

// Fetch page text for search. When *abortSearch is set, the caller should stop
// immediately (search was cancelled while engine locks were contended).
static Str GetTextForPageForSearch(EngineBase* engine, int pageNo, int* lenOut, const ProgressUpdateCb& progressCb,
                                   bool* abortSearch) {
    if (abortSearch) {
        *abortSearch = false;
    }
    if (!engine->TryGetTextForPage(pageNo, lenOut)) {
        if (WasCanceled(progressCb)) {
            if (abortSearch) {
                *abortSearch = true;
            }
            if (lenOut) {
                *lenOut = 0;
            }
            return {};
        }
        return engine->GetTextForPage(pageNo, lenOut);
    }
    return engine->GetTextForPage(pageNo, lenOut);
}

static void SkipWhitespace(Str text, int textLen, int& idx, int& byteIdx) {
    while (idx < textLen) {
        int nextByte = byteIdx;
        int c = Utf8CodepointNext(text, nextByte);
        if (!str::IsWs((char)c)) {
            break;
        }
        byteIdx = nextByte;
        idx++;
    }
}
// ignore spaces between CJK glyphs but not between Latin, Greek, Cyrillic, etc. letters
// cf. https://code.google.com/archive/p/sumatrapdf/issues/959
#define isnoncjkwordchar(c) (isWordChar(c) && (unsigned short)(c) < 0x2E80)

static void FoldCodepoints(Str text, int textLen, Vec<int>& out);

static void markAllPagesNonSkip(Vec<bool>& pagesToSkip) {
    for (int i = 0; i < len(pagesToSkip); i++) {
        pagesToSkip[i] = false;
    }
}
TextSearch::TextSearch(EngineBase* engine) : TextSelection(engine) {
    nPages = engine->PageCount();
    VecResize(pagesToSkip, nPages);
    markAllPagesNonSkip(pagesToSkip);
}

TextSearch::~TextSearch() {
    Clear();
}

void TextSearch::Clear() {
    str::FreePtr(&findText);
    str::FreePtr(&anchor);
    str::FreePtr(&lastText);
    findTextLen = 0;
    anchorLen = 0;
    VecReset(anchorFolded);
    Reset();
}

void TextSearch::Reset() {
    pageText = {};
    pageTextLen = 0;
    TextSelection::Reset();
}

int TextSearch::GetCurrentPageNo() const {
    return findPage;
}

// note: the result might not be a valid page number!
int TextSearch::GetSearchHitStartPageNo() const {
    return searchHitStartAt;
}

void TextSearch::SetText(Str text) {
    // search text starting with a single space enables the 'Match word start'
    // and search text ending in a single space enables the 'Match word end' option
    // (that behavior already "kind of" exists without special treatment, but
    // usually is not quite what a user expects, so let's try to be cleverer)
    // "match whole word" forces both word-boundary checks on; otherwise they're
    // driven by a leading / trailing single space in the search text
    this->matchWordStart = matchWholeWord || (text && text.s[0] == ' ' && (text.len < 2 || text.s[1] != ' '));
    this->matchWordEnd = matchWholeWord || (str::EndsWith(text, StrL(" ")) && !str::EndsWith(text, StrL("  ")));

    Str searchText = text;
    if (searchText && searchText.s[0] == ' ') {
        searchText = Str(searchText.s + 1, searchText.len - 1);
    }

    // don't reset anything if the search text hasn't changed at all
    if (str::Eq(this->lastText, searchText)) {
        return;
    }

    this->Clear();
    this->lastText = str::Dup(searchText);
    this->findText = str::Dup(searchText);
    this->findTextLen = Utf8CodepointCount(this->findText);

    // extract anchor string (the first word or the first symbol) for faster searching
    int searchTextLen = Utf8CodepointCount(searchText);
    int firstCharEndByte = 0;
    int firstChar = Utf8CodepointNext(searchText, firstCharEndByte);
    if (searchTextLen > 0 && isnoncjkwordchar(firstChar)) {
        int end = 1;
        int endByte = firstCharEndByte;
        while (end < searchTextLen) {
            int nextByte = endByte;
            int c = Utf8CodepointNext(searchText, nextByte);
            if (!isnoncjkwordchar(c)) {
                break;
            }
            endByte = nextByte;
            end++;
        }
        anchor = str::Dup(Str(searchText.s, endByte));
        anchorLen = end;
    }
    // Adobe Reader also matches certain hard-to-type Unicode
    // characters when searching for easy-to-type homoglyphs
    // cf. https://web.archive.org/web/20140201013717/http://forums.fofou.org:80/sumatrapdf/topic?id=2432337&comments=3
    // NOLINTNEXTLINE(bugprone-branch-clone): homoglyph case is distinct from the empty-anchor fallback
    else if (searchTextLen > 0 && (firstChar == '-' || firstChar == '\'' || firstChar == '"')) {
        anchor = {};
    } else if (searchTextLen > 0) {
        anchor = str::Dup(Str(searchText.s, firstCharEndByte));
        anchorLen = 1;
    } else {
        anchor = {};
    }

    VecReset(anchorFolded);

    if (anchor) {
        FoldCodepoints(anchor, anchorLen, anchorFolded);
    }

    if (str::EndsWith(this->findText, StrL(" "))) {
        this->findText.s[len(this->findText) - 1] = '\0';
        this->findText.len--;
        this->findTextLen--;
    }

    markAllPagesNonSkip(pagesToSkip);
}

void TextSearch::SetMatchCase(bool newMatchCase) {
    if (matchCase == newMatchCase) {
        return;
    }
    this->matchCase = newMatchCase;

    markAllPagesNonSkip(pagesToSkip);
}

void TextSearch::SetMatchWholeWord(bool newMatchWholeWord) {
    if (matchWholeWord == newMatchWholeWord) {
        return;
    }
    this->matchWholeWord = newMatchWholeWord;
    // matchWordStart/matchWordEnd are recomputed from matchWholeWord on the next
    // SetText() (the re-search after a toggle always calls it), so we only need
    // to invalidate the per-page skip cache here, like SetMatchCase().
    markAllPagesNonSkip(pagesToSkip);
}

bool TextSearch::PageAllowed(int pageNo) const {
    if (pageNo < 1 || pageNo > nPages) {
        return false;
    }
    if (len(pageAllowed) == 0) {
        return true;
    }
    if (pageNo > len(pageAllowed)) {
        return false;
    }
    return pageAllowed[pageNo - 1];
}

int TextSearch::RestrictFirst() const {
    if (len(pageAllowed) == 0) {
        return 1;
    }
    int n = std::min(len(pageAllowed), nPages);
    for (int i = 0; i < n; i++) {
        if (pageAllowed[i]) {
            return i + 1;
        }
    }
    return 1;
}

int TextSearch::RestrictLast() const {
    if (len(pageAllowed) == 0) {
        return nPages;
    }
    int last = 0;
    int n = std::min(len(pageAllowed), nPages);
    for (int i = 0; i < n; i++) {
        if (pageAllowed[i]) {
            last = i + 1;
        }
    }
    return last > 0 ? last : nPages;
}

void TextSearch::SetAllowedPages(const Vec<bool>& allowed) {
    pageAllowed = allowed;
    markAllPagesNonSkip(pagesToSkip);
}

void TextSearch::SetPageRange(int first, int last) {
    if (first < 0) {
        first = 0;
    }
    if (last < 0) {
        last = 0;
    }
    if (first == 0 && last == 0) {
        VecReset(pageAllowed);
        markAllPagesNonSkip(pagesToSkip);
        return;
    }
    int lo = first > 0 ? first : 1;
    int hi = last > 0 ? last : nPages;
    if (lo > hi) {
        int tmp = lo;
        lo = hi;
        hi = tmp;
    }
    Vec<bool> allowed;
    VecResize(allowed, nPages);
    for (int i = 0; i < nPages; i++) {
        int page = i + 1;
        allowed[i] = page >= lo && page <= hi;
    }
    SetAllowedPages(allowed);
}

void TextSearch::SetDirection(TextSearch::Direction direction) {
    bool fwd = TextSearch::Direction::Forward == direction;
    if (fwd == forward) {
        return;
    }
    forward = fwd;
    if (findText) {
        int n = findTextLen;
        if (fwd) {
            findIndex += n;
        } else {
            findIndex -= n;
        }
    }
}

void TextSearch::SetLastResult(TextSelection* sel) {
    CopySelection(sel);

    Str selection = ExtractText(StrL(" "));
    selection.len -= str::NormalizeWSInPlace(selection);
    SetText(selection);
    str::Free(selection);

    searchHitStartAt = findPage = std::min(startPage, endPage);
    findPage = std::max(startPage, endPage);
    findIndex = (findPage == endPage ? endGlyph : startGlyph);
    pageText = engine->GetTextForPage(findPage, &pageTextLen);
    forward = true;
}

#if !OS_WIN
static int FoldCaseWCharPortable(int c) {
    if (c >= L'A' && c <= L'Z') {
        return c + 32;
    }
    if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) {
        return c + 32;
    }
    if (c >= 0x0410 && c <= 0x042F) {
        return c + 32;
    }
    if (c == 0x0401) {
        return 0x0451;
    }
    if ((c >= 0x0391 && c <= 0x03A1) || (c >= 0x03A3 && c <= 0x03AB)) {
        return c + 32;
    }
    return (int)towlower((wint_t)c);
}
#endif

#if OS_WIN
// CharLowerW with an ASCII fast path. Most page text is ASCII and CharLowerW
// is an out-of-line user32 call per character.
static int FastCharLowerW(int c) {
    if (c < 0x80) {
        if (c >= 'A' && c <= 'Z') {
            return c + ('a' - 'A');
        }
        return c;
    }
    return (WCHAR)(uintptr_t)CharLowerW((LPWSTR)(uintptr_t)c);
}
#endif

// Locale-independent Unicode case folding for search. CharLowerW folds accented
// letters (e.g. É->é, Ş->ş) regardless of the CRT locale, unlike towlower() or
// the ASCII-only fast paths we used before.
static int FoldCaseForSearch(int c) {
    // U+0130 (İ, Latin capital I with dot above) lowercases to 'i' under
    // standard Unicode case folding, but CharLowerW only does this under a
    // Turkish system locale and otherwise leaves it unchanged -- so searching
    // "ibradı" wouldn't find "İbradı" on non-Turkish systems (issue #5597).
    // Fold it explicitly so search is case-insensitive regardless of locale.
    if (c == 0x0130) {
        return L'i';
    }
    if (c > 0 && c <= 0xffff) {
#if OS_WIN
        return FastCharLowerW(c);
#else
        return FoldCaseWCharPortable(c);
#endif
    }
    return c;
}

static const int kSharpS = 0x00DF;

// German ß (sharp s, U+00DF) is spelled "ss" and the two are often used
// interchangeably, so for case-insensitive search we treat ß as equivalent to
// "ss" (issue #933). Fold first so capital ẞ (U+1E9E) and case differences work.
static bool IsSharpS(int c) {
    return c != 0 && FoldCaseForSearch(c) == kSharpS;
}
static bool IsLatinS(int c) {
    return c != 0 && FoldCaseForSearch(c) == L's';
}

// case-fold every codepoint of text into out
static void FoldCodepoints(Str text, int textLen, Vec<int>& out) {
    VecResize(out, textLen);
    int* cps = out.els;
    int byteIdx = 0;
    for (int i = 0; i < textLen; i++) {
        // ASCII is a single byte and most page text; skip the decoder call
        int c = (u8)text.s[byteIdx];
        if (c < 0x80) {
            byteIdx++;
        } else {
            c = Utf8CodepointNext(text, byteIdx);
        }
        cps[i] = FoldCaseForSearch(c);
    }
}

// Compare one search "unit" of case-folded needle n against case-folded
// haystack h, treating ß as equivalent to "ss". On a match reports how many
// codepoints each side consumed (1:1, or 1:2 / 2:1 for ß <-> ss).
// h and n are raw codepoint arrays (Vec::els): this runs for every candidate
// position of every page, and operator[] bounds checks were a quarter of it
static bool MatchSearchUnit(const int* h, int hLen, int hIdx, const int* n, int nLen, int nIdx, int& hAdv, int& nAdv) {
    hAdv = nAdv = 0;
    if (hIdx >= hLen || nIdx >= nLen) {
        return false;
    }
    // h and n are already case-folded
    int hc = h[hIdx];
    int nc = n[nIdx];
    // ß in the needle matches "ss" in the text
    if (nc == kSharpS && hIdx + 1 < hLen && hc == L's') {
        if (h[hIdx + 1] == L's') {
            hAdv = 2;
            nAdv = 1;
            return true;
        }
    }
    // "ss" in the needle matches ß in the text
    if (nIdx + 1 < nLen && nc == L's' && hc == kSharpS) {
        if (n[nIdx + 1] == L's') {
            hAdv = 1;
            nAdv = 2;
            return true;
        }
    }
    // everything else (including ß~ß and ss~ss) matches one-to-one
    if (hc == nc) {
        hAdv = 1;
        nAdv = 1;
        return true;
    }
    return false;
}

static int StrStrFoldCase(const Vec<int>& haystackVec, int haystackLen, int startOff, const Vec<int>& needleVec,
                          int needleLen) {
    const int* haystack = haystackVec.els;
    const int* needle = needleVec.els;
    for (int i = startOff; i < haystackLen; i++) {
        int hIdx = i;
        int nIdx = 0;
        bool isMatch = true;
        while (nIdx < needleLen) {
            if (hIdx >= haystackLen) {
                isMatch = false;
                break;
            }
            int hAdv, nAdv;
            if (!MatchSearchUnit(haystack, haystackLen, hIdx, needle, needleLen, nIdx, hAdv, nAdv)) {
                isMatch = false;
                break;
            }
            hIdx += hAdv;
            nIdx += nAdv;
        }
        if (isMatch) {
            return i;
        }
    }
    return -1;
}

static bool StartsWithAtByte(Str text, int byteIdx, Str prefix) {
    return text && prefix && byteIdx >= 0 && byteIdx + prefix.len <= text.len &&
           memcmp(text.s + byteIdx, prefix.s, prefix.len) == 0;
}

static int StrRStr(Str text, int textLen, int endOff, Str needle, int needleLen) {
    if (!text || !needle || endOff <= 0 || endOff > textLen) {
        return -1;
    }
    if (needleLen <= 0 || needleLen > endOff) {
        return -1;
    }
    int result = -1;
    int byteIdx = 0;
    for (int i = 0; i <= endOff - needleLen; i++) {
        if (StartsWithAtByte(text, byteIdx, needle)) {
            result = i;
        }
        Utf8CodepointNext(text, byteIdx);
    }
    return result;
}

static int StrRStrFoldCase(const Vec<int>& textVec, int textLen, int endOff, const Vec<int>& needleVec, int needleLen) {
    if (endOff <= 0 || endOff > textLen) {
        return -1;
    }
    const int* text = textVec.els;
    const int* needle = needleVec.els;
    // ß <-> ss makes the matched length variable, so scan forward within
    // [start, end) and remember the last start position that matches.
    int result = -1;
    for (int i = 0; i < endOff; i++) {
        int hIdx = i;
        int nIdx = 0;
        bool isMatch = true;
        while (nIdx < needleLen) {
            if (hIdx >= endOff) {
                isMatch = false;
                break;
            }
            int hAdv, nAdv;
            if (!MatchSearchUnit(text, textLen, hIdx, needle, needleLen, nIdx, hAdv, nAdv)) {
                isMatch = false;
                break;
            }
            hIdx += hAdv;
            nIdx += nAdv;
        }
        if (isMatch) {
            result = i;
        }
    }
    return result;
}

// try to match "findText" from "start" with whitespace tolerance
// (ignore all whitespace except after alphanumeric characters)
TextSearch::PageAndOffset TextSearch::MatchEnd(int startOff) const {
    const PageAndOffset notFound = {-1, -1};
    int currentPage = findPage;
    Str currentPageText = pageText;
    int currentPageTextLen = pageTextLen;
    bool lookingAtWs;

    if (!findText) {
        return notFound;
    }

    int matchIdx = 0;
    int matchByteIdx = 0;
    int endIdx = startOff;
    int endByteIdx = Utf8CodepointToByteIndex(currentPageText, endIdx);

    if (matchWordStart && startOff > 0) {
        int prevByteIdx = endByteIdx;
        int prevCh = Utf8CodepointPrev(pageText, prevByteIdx);
        int nextByteIdx = endByteIdx;
        int curCh = Utf8CodepointNext(pageText, nextByteIdx);
        if (isWordChar(prevCh) && isWordChar(curCh)) {
            return notFound;
        }
    }

    while (matchIdx < findTextLen) {
        bool atPageEnd = endIdx >= currentPageTextLen;
        if (atPageEnd && currentPage >= nPages) {
            return notFound;
        }
        int endNextByteIdx = endByteIdx;
        int endCh = atPageEnd ? 0 : Utf8CodepointNext(currentPageText, endNextByteIdx);
        /* Going from page n to page n+1 is a space, too.*/
        lookingAtWs = (atPageEnd && (currentPage < nPages)) || str::IsWs((char)endCh);
        bool isMatch = false;
        // extra advance for the German ß <-> ss equivalence, where one side
        // consumes one codepoint and the other two (issue #933)
        int extraMatchAdv = 0;
        int extraEndAdv = 0;
        int matchNextByteIdx = matchByteIdx;
        int matchCh = Utf8CodepointNext(findText, matchNextByteIdx);
        if (matchCase) {
            isMatch = matchCh == endCh;
        } else {
            isMatch = FoldCaseForSearch(matchCh) == FoldCaseForSearch(endCh);
            if (!isMatch) {
                if (IsSharpS(matchCh) && !atPageEnd && endIdx + 1 < currentPageTextLen && IsLatinS(endCh)) {
                    int endAfterNextByteIdx = endNextByteIdx;
                    int nextEndCh = Utf8CodepointNext(currentPageText, endAfterNextByteIdx);
                    if (IsLatinS(nextEndCh)) {
                        // ß in the search text matches "ss" in the page
                        isMatch = true;
                        extraEndAdv = 1;
                        endNextByteIdx = endAfterNextByteIdx;
                    }
                } else if (matchIdx + 1 < findTextLen && IsLatinS(matchCh) && IsSharpS(endCh)) {
                    int matchAfterNextByteIdx = matchNextByteIdx;
                    int nextMatchCh = Utf8CodepointNext(findText, matchAfterNextByteIdx);
                    if (IsLatinS(nextMatchCh)) {
                        // "ss" in the search text matches ß in the page
                        isMatch = true;
                        extraMatchAdv = 1;
                        matchNextByteIdx = matchAfterNextByteIdx;
                    }
                }
            }
        }
        // NOLINTNEXTLINE(bugprone-branch-clone): each empty branch documents a different normalization
        if (isMatch) {
            /* characters are identical */;
        } else if (str::IsWs((char)matchCh) && lookingAtWs) {
            /* treat all whitespace as identical and end of page as whitespace.
               The end of the document is NOT seen as whitespace */
            ;
            // TODO: Adobe Reader seems to have a more extensive list of
            //       normalizations - is there an easier way?
        } else if (matchCh == L'-' && (0x2010 <= endCh && endCh <= 0x2014)) {
            /* make HYPHEN-MINUS also match HYPHEN, NON-BREAKING HYPHEN,
               FIGURE DASH, EN DASH and EM DASH (but not the other way around) */
            ;
        } else if (matchCh == L'\'' && (0x2018 <= endCh && endCh <= 0x201b)) {
            /* make APOSTROPHE also match LEFT/RIGHT SINGLE QUOTATION MARK */;
        } else if (matchCh == L'"' && (0x201c <= endCh && endCh <= 0x201f)) {
            /* make QUOTATION MARK also match LEFT/RIGHT DOUBLE QUOTATION MARK */;
        } else {
            return notFound;
        }
        // consume the extra char on whichever side of a ß <-> ss match is longer
        int matchAdv = 1 + extraMatchAdv;
        matchByteIdx = matchNextByteIdx;
        matchIdx += matchAdv;
        // We might get here either ...
        if (!atPageEnd && endCh) {
            // ... because there's a genuine match -> consider next character in next loop iteration
            int endAdv = 1 + extraEndAdv;
            endByteIdx = endNextByteIdx;
            endIdx += endAdv;
        } else {
            // ... or because we were looking at whitespace in the pattern and we were at a page break
            // -> skip to next page (but not past a restricted range)
            ++currentPage;
            if (!PageAllowed(currentPage)) {
                return notFound;
            }
            bool abortSearch = false;
            currentPageText =
                GetTextForPageForSearch(engine, currentPage, &currentPageTextLen, progressCb, &abortSearch);
            if (abortSearch) {
                return notFound;
            }
            endIdx = 0;
            endByteIdx = 0;
        }
        // treat "??" and "? ?" differently, since '?' could have been a word
        // character that's just missing an encoding (and '?' is the replacement
        // character); cf. https://code.google.com/archive/p/sumatrapdf/issues/1574
        int prevMatchByteIdx = matchByteIdx;
        int prevMatchCh = Utf8CodepointPrev(findText, prevMatchByteIdx);
        int curMatchCh = Utf8CodepointAtByte(findText, matchByteIdx);
        if (matchIdx < findTextLen && ((!isnoncjkwordchar(prevMatchCh) && (prevMatchCh != '?' || curMatchCh != '?')) ||
                                       (lookingAtWs && str::IsWs((char)prevMatchCh)))) {
            SkipWhitespace(findText, findTextLen, matchIdx, matchByteIdx);
            SkipWhitespace(currentPageText, currentPageTextLen, endIdx, endByteIdx);
            while (endIdx >= currentPageTextLen && PageAllowed(currentPage + 1)) {
                // treat page break as whitespace, too
                ++currentPage;
                bool abortSearch = false;
                currentPageText =
                    GetTextForPageForSearch(engine, currentPage, &currentPageTextLen, progressCb, &abortSearch);
                if (abortSearch) {
                    return notFound;
                }
                endIdx = 0;
                endByteIdx = 0;
                SkipWhitespace(currentPageText, currentPageTextLen, endIdx, endByteIdx);
            }
        }
    }
    if (matchWordEnd && endIdx > 0 && endIdx < currentPageTextLen) {
        int prevByteIdx = endByteIdx;
        int prevCh = Utf8CodepointPrev(currentPageText, prevByteIdx);
        int nextByteIdx = endByteIdx;
        int curCh = Utf8CodepointNext(currentPageText, nextByteIdx);
        if (isWordChar(prevCh) && isWordChar(curCh)) {
            return notFound;
        }
    }

    return {currentPage, endIdx};
}

static int StrStr(Str haystack, int haystackLen, int startOff, Str needle, int needleLen) {
    if (!haystack || len(needle) == 0) {
        return -1;
    }
    int byteIdx = Utf8CodepointToByteIndex(haystack, startOff);
    for (int i = startOff; i <= haystackLen - needleLen; i++) {
        if (StartsWithAtByte(haystack, byteIdx, needle)) {
            return i;
        }
        Utf8CodepointNext(haystack, byteIdx);
    }
    return -1;
}

static int GetNextIndex(int textLen, int offset, bool forward) {
    int idx = offset + (forward ? 0 : -1);
    if (idx < 0 || idx >= textLen) {
        return -1;
    }
    return idx;
}

bool TextSearch::FindTextInPage(int pageNo, TextSearch::PageAndOffset* finalGlyph) {
    if (len(findText) == 0) {
        return false;
    }
    if (!pageNo) {
        pageNo = findPage;
    }
    // According to my analysis of 69912675c766b6325f38036913dcf0505a00be36, when we
    // get here with pageNo != 0 the findText has already been set so I didn't add
    // a findText = engine->GetTextForPage(findPage) here.
    findPage = pageNo;

    // Perform case folding on the page text once in bulk.
    Vec<int> pageFolded;
    if (anchor && !matchCase) {
        FoldCodepoints(pageText, pageTextLen, pageFolded);
    }

    int found = -1;
    PageAndOffset fg;
    for (;;) {
        do {
            if (WasCanceled(progressCb)) {
                return false;
            }
            if (!anchor) {
                found = GetNextIndex(pageTextLen, findIndex, forward);
            } else if (forward) {
                if (matchCase) {
                    found = StrStr(pageText, pageTextLen, findIndex, anchor, anchorLen);
                } else {
                    found = StrStrFoldCase(pageFolded, pageTextLen, findIndex, anchorFolded, anchorLen);
                }
            } else {
                if (matchCase) {
                    found = StrRStr(pageText, pageTextLen, findIndex, anchor, anchorLen);
                } else {
                    found = StrRStrFoldCase(pageFolded, pageTextLen, findIndex, anchorFolded, anchorLen);
                }
            }
            if (found < 0) {
                return false;
            }
            findIndex = found + (forward ? 1 : 0);
            fg = MatchEnd(found);
        } while (fg.page <= 0);

        int offset = found;
        searchHitStartAt = pageNo;
        StartAt(pageNo, offset);
        SelectUpTo(fg.page, fg.offset);
        findIndex = forward ? fg.offset : offset;

        // try again if the found text is completely outside the page's mediabox
        if (result.len != 0) {
            break;
        }
    }

    if (finalGlyph) {
        *finalGlyph = fg;
    }
    return true;
}

// a chaptered doc may have laid out only the first chapter when this
// TextSearch was constructed; lay out the rest so a whole-document find
// covers every page, and grow pagesToSkip to match
void TextSearch::EnsureFullyLaidOut() {
    EnsureFullLayout(engine);
    int newPages = engine->PageCount();
    if (newPages == nPages) {
        return;
    }
    int oldPages = nPages;
    nPages = newPages;
    VecResize(pagesToSkip, nPages);
    for (int i = oldPages; i < nPages; i++) {
        pagesToSkip[i] = false;
    }
}

bool TextSearch::FindStartingAtPage(int pageNo) {
    if (len(findText) == 0) {
        return false;
    }

    EnsureFullyLaidOut();

    int lo = RestrictFirst();
    int hi = RestrictLast();
    if (pageNo < lo) {
        pageNo = forward ? lo : 0;
    } else if (pageNo > hi) {
        pageNo = forward ? nPages + 1 : hi;
    }

    int next = forward ? 1 : -1;
    while ((lo <= pageNo) && (pageNo <= hi) && !WasCanceled(progressCb)) {
        UpdateProgress(progressCb, pageNo, nPages);

        if (!PageAllowed(pageNo) || pagesToSkip[pageNo - 1]) {
            pageNo += next;
            continue;
        }

        Reset();

        bool abortSearch = false;
        pageText = GetTextForPageForSearch(engine, pageNo, &pageTextLen, progressCb, &abortSearch);
        if (abortSearch) {
            break;
        }
        findIndex = pageTextLen;
        if (!pageText) {
            pageNo += next;
            continue;
        }
        if (forward) {
            findIndex = 0;
        }
        PageAndOffset r;
        if (!FindTextInPage(pageNo, &r)) {
            pagesToSkip[pageNo - 1] = true;
            pageNo += next;
            continue;
        }
        if (forward) {
            if (findPage != r.page) {
                findPage = r.page;
                pageText = GetTextForPageForSearch(engine, findPage, &pageTextLen, progressCb, &abortSearch);
                if (abortSearch) {
                    break;
                }
            }
            findIndex = r.offset;
        }
        return true;
    }

    // allow for the first/last page of the (restricted) range to be included next
    searchHitStartAt = findPage = forward ? hi + 1 : lo - 1;

    return false;
}

TextSel* TextSearch::FindFirst(int page, Str text) {
    SetText(text);

    if (FindStartingAtPage(page)) {
        return &result;
    }
    return nullptr;
}

// search only `pageNo` (no wrapping to other pages), mirroring the per-page step
// inside FindStartingAtPage. Used for page-constrained search (issue #3085)
// like FindFirst but searches only the given page (issue #3085)
TextSel* TextSearch::FindFirstOnPage(int pageNo, Str text) {
    SetText(text);
    if (len(findText) == 0 || pageNo < 1 || pageNo > nPages) {
        return nullptr;
    }
    Reset();
    bool abortSearch = false;
    pageText = GetTextForPageForSearch(engine, pageNo, &pageTextLen, progressCb, &abortSearch);
    if (abortSearch) {
        return nullptr;
    }
    findIndex = pageTextLen;
    if (!pageText) {
        return nullptr;
    }
    if (forward) {
        findIndex = 0;
    }
    PageAndOffset r;
    if (!FindTextInPage(pageNo, &r)) {
        return nullptr;
    }
    if (forward) {
        if (findPage != r.page) {
            findPage = r.page;
            pageText = GetTextForPageForSearch(engine, findPage, &pageTextLen, progressCb, &abortSearch);
            if (abortSearch) {
                return nullptr;
            }
        }
        findIndex = r.offset;
    }
    return &result;
}

TextSel* TextSearch::FindNext() {
    ReportIf(!findText);
    if (!findText) {
        return nullptr;
    }

    if (WasCanceled(progressCb)) {
        return nullptr;
    }
    UpdateProgress(progressCb, findPage, nPages);

    PageAndOffset finalGlyph;
    if (FindTextInPage(findPage, &finalGlyph)) {
        if (forward) {
            findPage = finalGlyph.page;
            findIndex = finalGlyph.offset;
            bool abortSearch = false;
            pageText = GetTextForPageForSearch(engine, findPage, &pageTextLen, progressCb, &abortSearch);
            if (abortSearch) {
                return nullptr;
            }
        }
        return &result;
    }

    auto next = forward ? 1 : -1;
    if (FindStartingAtPage(findPage + next)) {
        return &result;
    }
    return nullptr;
}
