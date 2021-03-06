/******************************************************************
 *  LexMarkdown.cxx
 *
 *  A simple Markdown lexer for scintilla.
 *
 *  Includes highlighting for some extra features from the
 *  Pandoc implementation; strikeout, using '#.' as a default
 *  ordered list item marker, and delimited code blocks.
 *
 *  Limitations:
 *
 *  Standard indented code blocks are not highlighted at all,
 *  as it would conflict with other indentation schemes. Use
 *  delimited code blocks for blanket highlighting of an
 *  entire code block.  Embedded HTML is not highlighted either.
 *  Blanket HTML highlighting has issues, because some Markdown
 *  implementations allow Markdown markup inside of the HTML. Also,
 *  there is a following blank line issue that can't be ignored,
 *  explained in the next paragraph. Embedded HTML and code
 *  blocks would be better supported with language specific
 *  highlighting.
 *
 *  The highlighting aims to accurately reflect correct syntax,
 *  but a few restrictions are relaxed. Delimited code blocks are
 *  highlighted, even if the line following the code block is not blank.
 *  Requiring a blank line after a block, breaks the highlighting
 *  in certain cases, because of the way Scintilla ends up calling
 *  the lexer.
 *
 *  Written by Jon Strait - jstrait@moonloop.net
 *
 *  The License.txt file describes the conditions under which this
 *  software may be distributed.
 *
 *****************************************************************/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"

#include "WordList.h"
#include "LexAccessor.h"
#include "Accessor.h"
#include "StyleContext.h"
#include "CharacterSet.h"
#include "LexerModule.h"

using namespace Scintilla;

static inline bool IsNewline(const int ch) {
    return (ch == '\n' || ch == '\r');
}

// True if can follow ch down to the end with possibly trailing whitespace
static bool FollowToLineEnd(const int ch, const int state, const Sci_PositionU endPos, StyleContext &sc) {
    Sci_PositionU i = 0;
    while (sc.GetRelative(++i) == ch)
        ;
    // Skip over whitespace
    while (IsASpaceOrTab(sc.GetRelative(i)) && sc.currentPos + i < endPos)
        ++i;
    if (IsNewline(sc.GetRelative(i)) || sc.currentPos + i == endPos) {
        sc.Forward(i);
        sc.ChangeState(state);
        sc.SetState(SCE_MARKDOWN_LINE_BEGIN);
        return true;
    }
    else return false;
}

// Does the previous line have more than spaces and tabs?
static bool HasPrevLineContent(StyleContext &sc) {
    Sci_Position i = 0;

	// Go back to the previous newline
    while ((--i + (Sci_Position)sc.currentPos) >= 0 && !IsNewline(sc.GetRelative(i)))
        ;

	// Skip newline characters
	int ch = sc.GetRelative(i);
	int prevCh = sc.GetRelative(i - 1);
	if (prevCh == '\r' && ch == '\n')
		--i;

	while ((--i + (Sci_Position)sc.currentPos) >= 0) {
        if (IsNewline(sc.GetRelative(i)))
            break;
        if (!IsASpaceOrTab(sc.GetRelative(i)))
            return true;
    }
    return false;
}

// Returns true if the previous line contains only newline characters
static bool IsPrevLineEmpty(StyleContext &sc) {
	int i = 0;

	// Go back to the previous newline
	while ((--i + (int)sc.currentPos) >= 0 && !IsNewline(sc.GetRelative(i)))
		;

	// Skip newline characters
	int ch = sc.GetRelative(i);
	int prevCh = sc.GetRelative(i - 1);
	if (prevCh == '\r' && ch == '\n')
		i -= 2;
	else if (ch == '\r' || ch == '\n')
		i -= 1;

	return IsNewline(sc.GetRelative(i)) || (int)sc.currentPos + i <= 0;
}

static bool AtTermStart(StyleContext &sc) {
    return sc.currentPos == 0 || sc.chPrev == 0 || isspacechar(sc.chPrev);
}

static bool IsValidHrule(const Sci_PositionU endPos, StyleContext &sc) {
    int count = 1;
    Sci_PositionU i = 0;
    for (;;) {
        ++i;
        int c = sc.GetRelative(i);
        if (c == sc.ch)
            ++count;
        // hit a terminating character
        else if (!IsASpaceOrTab(c) || sc.currentPos + i == endPos) {
            // Are we a valid HRULE
            if ((IsNewline(c) || sc.currentPos + i == endPos) &&
                    count >= 3 && !HasPrevLineContent(sc)) {
                sc.SetState(SCE_MARKDOWN_HRULE);
                sc.Forward(i);
                sc.SetState(SCE_MARKDOWN_LINE_BEGIN);
                return true;
            }
            else {
                sc.SetState(SCE_MARKDOWN_DEFAULT);
		return false;
            }
        }
    }
}

// Check if we are at the beginning of the first line of a header underlined with '=' or '-'
static bool IsUnderlinedHeader(const int hdrCh, const unsigned int endPos, StyleContext &sc) {
	unsigned int i = 0;

	// Go to the end of the first line and check if it has any content (non-whitespace characters)
	int ch;
	bool firstLineHasContent = false;
	while (sc.currentPos + i < endPos && !IsNewline(ch = sc.GetRelative(i))) {
		if (!IsASpaceOrTab(ch))
			firstLineHasContent = true;
		++i;
	}

	ch = sc.GetRelative(i);
	if (!firstLineHasContent || !IsNewline(ch))
		return false;

	// Skip newline characters
	int nextCh = sc.GetRelative(i + 1);
	i += (ch == '\r' && nextCh == '\n') ? 2 : 1;

	// Check if the second line starts with '=' or '-' characters and skip them
	if (sc.GetRelative(i) != hdrCh)
		return false;
	while (sc.currentPos + i < endPos && sc.GetRelative(i) == hdrCh)
		++i;

	// Skip whitespace
	while (sc.currentPos + i < endPos && IsASpaceOrTab(sc.GetRelative(i)))
		++i;

	// Check if we are at the end of line
	return IsNewline(sc.GetRelative(i)) || sc.currentPos + i == endPos;
}

static void ColorizeMarkdownDoc(Sci_PositionU startPos, Sci_Position length, int initStyle,
                               WordList **, Accessor &styler) {
    Sci_PositionU endPos = startPos + length;
    int precharCount = 0;
    bool isLinkNameDetecting = false;
    // Don't advance on a new loop iteration and retry at the same position.
    // Useful in the corner case of having to start at the beginning file position
    // in the default state.
    bool freezeCursor = false;

	// Start from the previous line in order to get headers underlined with '=' or '-' to work
	if (startPos > 0) {
		unsigned int newStartPos = styler.LineStart(styler.GetLine(startPos) - 1);
		length += startPos - newStartPos;
		startPos = newStartPos;
		styler.StartAt(startPos);
		initStyle = styler.StyleAt(startPos);
	}

	// Do not leak URL highlighting onto the next line
	if (initStyle > SCE_MARKDOWN_CODEBK)
		initStyle = SCE_MARKDOWN_DEFAULT;

    StyleContext sc(startPos, length, initStyle, styler);

    while (sc.More()) {
        // Skip past escaped characters
        if (sc.ch == '\\') {
            sc.Forward();
            continue;
        }

        // A blockquotes resets the line semantics
        if (sc.state == SCE_MARKDOWN_BLOCKQUOTE)
            sc.SetState(SCE_MARKDOWN_LINE_BEGIN);

        // Conditional state-based actions
        if (sc.state == SCE_MARKDOWN_CODE2) {
            if (sc.Match("```")) {
                sc.Forward(3);
                sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
        }
        else if (sc.state == SCE_MARKDOWN_CODE) {
            if (sc.ch == '`' && sc.chPrev != ' ')
                sc.ForwardSetState(SCE_MARKDOWN_DEFAULT);
        }
        // Code block
        else if (sc.state == SCE_MARKDOWN_CODEBK) {
            bool d = true;
			if (sc.atLineStart && sc.ch != '\t') {
				for (int i = 0; i < 4; ++i) {
					if (sc.GetRelative(i) != ' ')
						d = false;
				}
			}
            if (!d)
                sc.SetState(SCE_MARKDOWN_LINE_BEGIN);
        }
        // Strong
        else if (sc.state == SCE_MARKDOWN_STRONG1) {
            if (sc.Match("**") && sc.chPrev != ' ') {
                sc.Forward(2);
                sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
        }
        else if (sc.state == SCE_MARKDOWN_STRONG2) {
            if (sc.Match("__") && sc.chPrev != ' ') {
                sc.Forward(2);
                sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
        }
        // Emphasis
		/*
        else if (sc.state == SCE_MARKDOWN_EM1) {
            if (sc.ch == '*' && sc.chPrev != ' ')
                sc.ForwardSetState(SCE_MARKDOWN_DEFAULT);
        }
        else if (sc.state == SCE_MARKDOWN_EM2) {
            if (sc.ch == '_' && sc.chPrev != ' ')
                sc.ForwardSetState(SCE_MARKDOWN_DEFAULT);
        }
		*/
        else if (sc.state == SCE_MARKDOWN_CODEBK) {
            if (sc.atLineStart && sc.Match("~~~")) {
                Sci_Position i = 1;
                while (!IsNewline(sc.GetRelative(i)) && sc.currentPos + i < endPos)
                    i++;
                sc.Forward(i);
                sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
        }
        else if (sc.state == SCE_MARKDOWN_STRIKEOUT) {
            if (sc.Match("~~") && sc.chPrev != ' ') {
                sc.Forward(2);
                sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
        }
        else if (sc.state == SCE_MARKDOWN_LINE_BEGIN) {
            // Header starting with '#'
			if (sc.Match("######"))
				sc.SetState(SCE_MARKDOWN_HEADER6);
            else if (sc.Match("#####"))
				sc.SetState(SCE_MARKDOWN_HEADER5);
            else if (sc.Match("####"))
				sc.SetState(SCE_MARKDOWN_HEADER4);
            else if (sc.Match("###"))
				sc.SetState(SCE_MARKDOWN_HEADER3);
            else if (sc.Match("##"))
				sc.SetState(SCE_MARKDOWN_HEADER2);
            else if (sc.Match("#")) {
				/*
                // Catch the special case of an unordered list
                if (sc.chNext == '.' && IsASpaceOrTab(sc.GetRelative(2))) {
                    precharCount = 0;
                    sc.SetState(SCE_MARKDOWN_PRECHAR);
                }
                else
				*/
				sc.SetState(SCE_MARKDOWN_HEADER1);
            }
            // Code block
            else if (sc.Match("~~~")) {
                if (!HasPrevLineContent(sc))
                    sc.SetState(SCE_MARKDOWN_CODEBK);
                else
                    sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
			// Header underlined with '='
			else if (IsUnderlinedHeader('=', endPos, sc))
				sc.SetState(SCE_MARKDOWN_HEADER1);
            else if (sc.ch == '=') {
                if (HasPrevLineContent(sc) && FollowToLineEnd('=', SCE_MARKDOWN_HEADER1, endPos, sc))
					;
                else
                    sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
			// Header underlined with '-'
			else if (IsUnderlinedHeader('-', endPos, sc))
				sc.SetState(SCE_MARKDOWN_HEADER2);
			else if (sc.ch == '-') {
				if (HasPrevLineContent(sc) && FollowToLineEnd('-', SCE_MARKDOWN_HEADER2, endPos, sc))
                    ;
                else {
                    precharCount = 0;
                    sc.SetState(SCE_MARKDOWN_PRECHAR);
                }
            }
            else if (IsNewline(sc.ch))
                sc.SetState(SCE_MARKDOWN_LINE_BEGIN);
            else {
                precharCount = 0;
                sc.SetState(SCE_MARKDOWN_PRECHAR);
            }
        }

        // The header lasts until the newline
        else if (sc.state == SCE_MARKDOWN_HEADER1 || sc.state == SCE_MARKDOWN_HEADER2 ||
                sc.state == SCE_MARKDOWN_HEADER3 || sc.state == SCE_MARKDOWN_HEADER4 ||
                sc.state == SCE_MARKDOWN_HEADER5 || sc.state == SCE_MARKDOWN_HEADER6) {
            if (IsNewline(sc.ch))
                sc.SetState(SCE_MARKDOWN_LINE_BEGIN);
        }

        // New state only within the initial whitespace
        if (sc.state == SCE_MARKDOWN_PRECHAR) {
            // Blockquote
            if (sc.ch == '>' && precharCount < 5)
                sc.SetState(SCE_MARKDOWN_BLOCKQUOTE);
            // Begin of code block
            else if (IsPrevLineEmpty(sc) && (sc.chPrev == '\t' || precharCount >= 4))
                sc.SetState(SCE_MARKDOWN_CODEBK);
            // HRule - Total of three or more hyphens, asterisks, or underscores
            // on a line by themselves
            else if ((sc.ch == '-' || sc.ch == '*' || sc.ch == '_') && IsValidHrule(endPos, sc))
                ;
			/*
            // Unordered list
            else if ((sc.ch == '-' || sc.ch == '*' || sc.ch == '+') && IsASpaceOrTab(sc.chNext)) {
                sc.SetState(SCE_MARKDOWN_ULIST_ITEM);
                sc.ForwardSetState(SCE_MARKDOWN_DEFAULT);
            }
            // Ordered list
            else if (IsADigit(sc.ch)) {
                int digitCount = 0;
                while (IsADigit(sc.GetRelative(++digitCount)))
                    ;
                if (sc.GetRelative(digitCount) == '.' &&
                        IsASpaceOrTab(sc.GetRelative(digitCount + 1))) {
                    sc.SetState(SCE_MARKDOWN_OLIST_ITEM);
                    sc.Forward(digitCount + 1);
                    sc.SetState(SCE_MARKDOWN_DEFAULT);
                }
            }
            // Alternate Ordered list
            else if (sc.ch == '#' && sc.chNext == '.' && IsASpaceOrTab(sc.GetRelative(2))) {
                sc.SetState(SCE_MARKDOWN_OLIST_ITEM);
                sc.Forward(2);
                sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
			*/
            else if (sc.ch != ' ')
                sc.SetState(SCE_MARKDOWN_DEFAULT);
            else
                ++precharCount;
        }

        // Any link
        if (sc.state == SCE_MARKDOWN_LINK) {
            if (sc.Match("](") && sc.GetRelative(-1) != '\\') {
              sc.Forward(2);
              isLinkNameDetecting = true;
            }
            else if (sc.Match("]:") && sc.GetRelative(-1) != '\\') {
              sc.Forward(2);
              sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
            else if (!isLinkNameDetecting && sc.ch == ']' && sc.GetRelative(-1) != '\\') {
              sc.Forward();
              sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
            else if (isLinkNameDetecting && sc.ch == ')' && sc.GetRelative(-1) != '\\') {
              sc.Forward();
              sc.SetState(SCE_MARKDOWN_DEFAULT);
              isLinkNameDetecting = false;
            }
        }

        // New state anywhere in doc
        if (sc.state == SCE_MARKDOWN_DEFAULT) {
            if (sc.atLineStart && (sc.ch == '#' ||
								   IsUnderlinedHeader('=', endPos, sc) ||
								   IsUnderlinedHeader('-', endPos, sc))) {
                sc.SetState(SCE_MARKDOWN_LINE_BEGIN);
                freezeCursor = true;
            }
            // Links and Images
			/*
              sc.SetState(SCE_MARKDOWN_LINK);
              sc.Forward(2);
            }
            else if (sc.ch == '[' && sc.GetRelative(-1) != '\\') {
              sc.SetState(SCE_MARKDOWN_LINK);
              sc.Forward();
            }
			*/
            // Code - also a special case for alternate inside spacing
            else if (sc.Match("```") && AtTermStart(sc)) {
                sc.SetState(SCE_MARKDOWN_CODE2);
                sc.Forward();
            }
            else if (sc.ch == '`' && sc.chNext != ' ' && AtTermStart(sc)) {
                sc.SetState(SCE_MARKDOWN_CODE);
            }
            // Strong
            else if (sc.Match("**") && sc.GetRelative(2) != ' ' && AtTermStart(sc)) {
                sc.SetState(SCE_MARKDOWN_STRONG1);
                sc.Forward();
           }
            else if (sc.Match("__") && sc.GetRelative(2) != ' ' && AtTermStart(sc)) {
                sc.SetState(SCE_MARKDOWN_STRONG2);
                sc.Forward();
            }
            // Emphasis
			/*
            else if (sc.ch == '*' && sc.chNext != ' ' && AtTermStart(sc)) {
                sc.SetState(SCE_MARKDOWN_EM1);
            }
            else if (sc.ch == '_' && sc.chNext != ' ' && AtTermStart(sc)) {
                sc.SetState(SCE_MARKDOWN_EM2);
            }
			*/
            // Strikeout
            else if (sc.Match("~~") && sc.GetRelative(2) != ' ' && AtTermStart(sc)) {
                sc.SetState(SCE_MARKDOWN_STRIKEOUT);
                sc.Forward();
            }
            // Beginning of line
            else if (IsNewline(sc.ch)) {
                sc.SetState(SCE_MARKDOWN_LINE_BEGIN);
            }
        }
        // Advance if not holding back the cursor for this iteration.
        if (!freezeCursor)
            sc.Forward();
        freezeCursor = false;
    }
    sc.Complete();
}

LexerModule lmMarkdown(SCLEX_MARKDOWN, ColorizeMarkdownDoc, "markdown");
