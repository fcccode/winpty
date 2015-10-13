// Copyright (c) 2011-2012 Ryan Prichard
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

#include "Terminal.h"
#include "NamedPipe.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "../shared/DebugClient.h"

#define CSI "\x1b["

const int COLOR_ATTRIBUTE_MASK =
        FOREGROUND_BLUE |
        FOREGROUND_GREEN |
        FOREGROUND_RED |
        FOREGROUND_INTENSITY |
        BACKGROUND_BLUE |
        BACKGROUND_GREEN |
        BACKGROUND_RED |
        BACKGROUND_INTENSITY;

const int FLAG_RED    = 1;
const int FLAG_GREEN  = 2;
const int FLAG_BLUE   = 4;
const int FLAG_BRIGHT = 8;

const int BLACK  = 0;
const int DKGRAY = BLACK | FLAG_BRIGHT;
const int LTGRAY = FLAG_RED | FLAG_GREEN | FLAG_BLUE;
const int WHITE  = LTGRAY | FLAG_BRIGHT;

// SGR parameters (Select Graphic Rendition)
const int SGR_FORE = 30;
const int SGR_FORE_HI = 90;
const int SGR_BACK = 40;
const int SGR_BACK_HI = 100;

// Work around the old MinGW, which lacks COMMON_LVB_LEADING_BYTE and
// COMMON_LVB_TRAILING_BYTE.
const int WINPTY_COMMON_LVB_LEADING_BYTE = 0x100;
const int WINPTY_COMMON_LVB_TRAILING_BYTE = 0x200;

namespace {

static void outUInt(std::string &out, unsigned int n)
{
    char buf[32];
    char *pbuf = &buf[32];
    *(--pbuf) = '\0';
    do {
        *(--pbuf) = '0' + n % 10;
        n /= 10;
    } while (n != 0);
    out.append(pbuf);
}

static void outputSetColorSgrParams(std::string &out, bool isFore, int color)
{
    out.push_back(';');
    const int sgrBase = isFore ? SGR_FORE : SGR_BACK;
    if (color & FLAG_BRIGHT) {
        // Some terminals don't support the 9X/10X "intensive" color parameters
        // (e.g. the Eclipse TM terminal as of this writing).  Those terminals
        // will quietly ignore a 9X/10X code, and the other terminals will
        // ignore a 3X/4X code if it's followed by a 9X/10X code.  Therefore,
        // output a 3X/4X code as a fallback, then override it.
        const int colorBase = color & ~FLAG_BRIGHT;
        outUInt(out, sgrBase + colorBase);
        out.push_back(';');
        outUInt(out, sgrBase + (SGR_FORE_HI - SGR_FORE) + colorBase);
    } else {
        outUInt(out, sgrBase + color);
    }
}

} // anonymous namespace

Terminal::Terminal(NamedPipe *output) :
    m_output(output),
    m_remoteLine(0),
    m_cursorHidden(false),
    m_remoteColor(-1),
    m_consoleMode(false)
{
}

void Terminal::setConsoleMode(int mode)
{
    if (mode == 1)
        m_consoleMode = true;
    else
        m_consoleMode = false;
}

void Terminal::reset(bool sendClearFirst, int newLine)
{
    if (sendClearFirst && !m_consoleMode) {
        // 0m   ==> reset SGR parameters
        // 1;1H ==> move cursor to top-left position
        // 2J   ==> clear the entire screen
        m_output->write(CSI"0m" CSI"1;1H" CSI"2J");
    }
    m_remoteLine = newLine;
    m_cursorHidden = false;
    m_cursorPos = std::pair<int, int>(0, newLine);
    m_remoteColor = -1;
}

void Terminal::sendLine(int line, CHAR_INFO *lineData, int width)
{
    hideTerminalCursor();
    moveTerminalToLine(line);

    // Erase in Line -- erase entire line.
    if (!m_consoleMode)
        m_output->write(CSI"2K");

    m_termLine.clear();

    int length = 0;
    for (int i = 0; i < width; ++i) {
        int color = lineData[i].Attributes & COLOR_ATTRIBUTE_MASK;
        if (color != m_remoteColor && !m_consoleMode) {
            int fore = 0;
            int back = 0;
            if (color & FOREGROUND_RED)       fore |= FLAG_RED;
            if (color & FOREGROUND_GREEN)     fore |= FLAG_GREEN;
            if (color & FOREGROUND_BLUE)      fore |= FLAG_BLUE;
            if (color & FOREGROUND_INTENSITY) fore |= FLAG_BRIGHT;
            if (color & BACKGROUND_RED)       back |= FLAG_RED;
            if (color & BACKGROUND_GREEN)     back |= FLAG_GREEN;
            if (color & BACKGROUND_BLUE)      back |= FLAG_BLUE;
            if (color & BACKGROUND_INTENSITY) back |= FLAG_BRIGHT;

            // Translate the fore/back colors into terminal escape codes using
            // a heuristic that works OK with common white-on-black or
            // black-on-white color schemes.  We don't know which color scheme
            // the terminal is using.  It is ugly to force white-on-black text
            // on a black-on-white terminal, and it's even ugly to force the
            // matching scheme.  It's probably relevant that the default
            // fore/back terminal colors frequently do not match any of the 16
            // palette colors.

            // Typical default terminal color schemes (according to palette,
            // when possible):
            //  - mintty:               LtGray-on-Black(A)
            //  - putty:                LtGray-on-Black(A)
            //  - xterm:                LtGray-on-Black(A)
            //  - Konsole:              LtGray-on-Black(A)
            //  - JediTerm/JetBrains:   Black-on-White(B)
            //  - rxvt:                 Black-on-White(B)

            // If the background is the default color (black), then it will
            // map to Black(A) or White(B).  If we translate White to White,
            // then a Black background and a White background in the console
            // are both White with (B).  Therefore, we should translate White
            // using SGR 7 (Invert).  The typical finished mapping table for
            // background grayscale colors is:
            //
            //  (A) White => LtGray(fore)
            //  (A) Black => Black(back)
            //  (A) LtGray => LtGray
            //  (A) DkGray => DkGray
            //
            //  (B) White => Black(fore)
            //  (B) Black => White(back)
            //  (B) LtGray => LtGray
            //  (B) DkGray => DkGray
            //

            m_termLine.append(CSI"0");
            if (back == BLACK) {
                if (fore == LTGRAY) {
                    // The "default" foreground color.  Use the terminal's
                    // default colors.
                } else if (fore == WHITE) {
                    // Sending the literal color white would behave poorly if
                    // the terminal were black-on-white.  Sending Bold is not
                    // guaranteed to alter the color, but it will make the text
                    // visually distinct, so do that instead.
                    m_termLine.append(";1");
                } else if (fore == DKGRAY) {
                    // Set the foreground color to DkGray(90) with a fallback
                    // of LtGray(37) for terminals that don't handle the 9X SGR
                    // parameters (e.g. Eclipse's TM Terminal as of this
                    // writing).
                    m_termLine.append(";37;90");
                } else {
                    outputSetColorSgrParams(m_termLine, true, fore);
                }
            } else if (back == WHITE) {
                // Set the background color using Invert on the default
                // foreground color, and set the foreground color by setting a
                // background color.

                // Use the terminal's inverted colors.
                m_termLine.append(";7");
                if (fore == LTGRAY || fore == BLACK) {
                    // We're likely mapping Console White to terminal LtGray or
                    // Black.  If they are the Console foreground color, then
                    // don't set a terminal foreground color to avoid creating
                    // invisible text.
                } else {
                    outputSetColorSgrParams(m_termLine, false, fore);
                }
            } else {
                // Set the foreground and background to match exactly that in
                // the Windows console.
                outputSetColorSgrParams(m_termLine, true, fore);
                outputSetColorSgrParams(m_termLine, false, back);
            }
            if (fore == back) {
                // The foreground and background colors are exactly equal, so
                // attempt to hide the text using the Conceal SGR parameter,
                // which some terminals support.
                m_termLine.append(";8");
            }
            m_termLine.push_back('m');
            length = m_termLine.size();
        }
        m_remoteColor = color;
        if (lineData[i].Attributes & WINPTY_COMMON_LVB_TRAILING_BYTE) {
            // CJK full-width characters occupy two console cells.  The first
            // cell is marked with COMMON_LVB_LEADING_BYTE, and the second is
            // marked with COMMON_LVB_TRAILING_BYTE.  Skip the trailing cells.
            continue;
        }
        // TODO: Is it inefficient to call WideCharToMultiByte once per
        // character?

        wchar_t ch = lineData[i].Char.UnicodeChar;

        // The Windows Console has a popup window (e.g. that appears with F7)
        // that is sometimes bordered with box-drawing characters.  With the
        // Japanese and Korean system locales (CP932 and CP949), the
        // UnicodeChar values for the box-drawing characters are 1..6.  Detect
        // this and map the values to the correct Unicode values.
        //
        // N.B. In the English locale, the UnicodeChar values are correct, and
        // they identify single-line characters rather than double-line.  In
        // the Chinese Simplified and Traditional locales, the popups use ASCII
        // characters instead.
        if (ch <= 6) {
            switch (ch) {
                case 1: ch = 0x2554; break; // BOX DRAWINGS DOUBLE DOWN AND RIGHT
                case 2: ch = 0x2557; break; // BOX DRAWINGS DOUBLE DOWN AND LEFT
                case 3: ch = 0x255A; break; // BOX DRAWINGS DOUBLE UP AND RIGHT
                case 4: ch = 0x255D; break; // BOX DRAWINGS DOUBLE UP AND LEFT
                case 5: ch = 0x2551; break; // BOX DRAWINGS DOUBLE VERTICAL
                case 6: ch = 0x2550; break; // BOX DRAWINGS DOUBLE HORIZONTAL
            }
        }

        char mbstr[16];
        int mblen = WideCharToMultiByte(CP_UTF8,
                                        0,
                                        &ch,
                                        1,
                                        mbstr,
                                        sizeof(mbstr),
                                        NULL,
                                        NULL);
        if (mblen <= 0) {
            mbstr[0] = '?';
            mblen = 1;
        }
        if (mblen == 1 && mbstr[0] == ' ') {
            m_termLine.push_back(' ');
        } else {
            m_termLine.append(mbstr, mblen);
            length = m_termLine.size();
        }
    }

    m_output->write(m_termLine.data(), length);
}

void Terminal::finishOutput(const std::pair<int, int> &newCursorPos)
{
    if (newCursorPos != m_cursorPos)
        hideTerminalCursor();
    if (m_cursorHidden) {
        moveTerminalToLine(newCursorPos.second);
        char buffer[32];
        sprintf(buffer, CSI"%dG" CSI"?25h", newCursorPos.first + 1);
        if (!m_consoleMode)
            m_output->write(buffer);
        m_cursorHidden = false;
    }
    m_cursorPos = newCursorPos;
}

void Terminal::hideTerminalCursor()
{
    if (m_cursorHidden)
        return;
    if (!m_consoleMode)
        m_output->write(CSI"?25l");
    m_cursorHidden = true;
}

void Terminal::moveTerminalToLine(int line)
{
    // Do not use CPL or CNL.  Konsole 2.5.4 does not support Cursor Previous
    // Line (CPL) -- there are "Undecodable sequence" errors.  gnome-terminal
    // 2.32.0 does handle it.  Cursor Next Line (CNL) does nothing if the
    // cursor is on the last line already.

    if (line < m_remoteLine) {
        // CUrsor Up (CUU)
        char buffer[32];
        sprintf(buffer, "\r" CSI"%dA", m_remoteLine - line);
        if (!m_consoleMode)
            m_output->write(buffer);
        m_remoteLine = line;
    } else if (line > m_remoteLine) {
        while (line > m_remoteLine) {
            if (!m_consoleMode)
                m_output->write("\r\n");
            m_remoteLine++;
        }
    } else {
        m_output->write("\r");
    }
}
