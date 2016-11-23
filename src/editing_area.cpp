﻿/*
 *  This file is part of Poedit (https://poedit.net)
 *
 *  Copyright (C) 1999-2016 Vaclav Slavik
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *  DEALINGS IN THE SOFTWARE.
 *
 */

#include "editing_area.h"

#include "colorscheme.h"
#include "customcontrols.h"
#include "edlistctrl.h"
#include "errorbar.h"
#include "hidpi.h"
#include "main_toolbar.h"
#include "pluralforms/pl_evaluate.h"
#include "spellchecking.h"
#include "text_control.h"
#include "utility.h"

#include <wx/button.h>
#include <wx/dcclient.h>
#include <wx/graphics.h>
#include <wx/notebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <algorithm>


namespace
{

struct EventHandlerDisabler
{
    EventHandlerDisabler(wxEvtHandler *h) : m_hnd(h)
        { m_hnd->SetEvtHandlerEnabled(false); }
    ~EventHandlerDisabler()
        { m_hnd->SetEvtHandlerEnabled(true); }

    wxEvtHandler *m_hnd;
};

void SetTranslationValue(TranslationTextCtrl *txt, const wxString& value, int flags)
{
    // disable EVT_TEXT forwarding -- the event is generated by
    // programmatic changes to text controls' content and we *don't*
    // want UpdateFromTextCtrl() to be called from here
    EventHandlerDisabler disabler(txt->GetEventHandler());

    if (flags & EditingArea::UndoableEdit)
        txt->SetPlainTextUserWritten(value);
    else
        txt->SetPlainText(value);
}

inline void SetCtrlFont(wxWindow *win, const wxFont& font)
{
    if (!win)
        return;

#ifdef __WXMSW__
    // Native wxMSW text control sends EN_CHANGE when the font changes,
    // producing a wxEVT_TEXT event as if the user changed the value.
    // Unfortunately the event seems to be used internally for sizing,
    // so we can't just filter it out completely. What we can do, however,
    // is to disable *our* handling of the event.
    EventHandlerDisabler disabler(win->GetEventHandler());
#endif
    win->SetFont(font);
}

// does some basic processing of user input, e.g. to remove trailing \n
wxString PreprocessEnteredTextForItem(CatalogItemPtr item, wxString t)
{
    auto& orig = item->GetString();

    if (!t.empty() && !orig.empty())
    {
        if (orig.Last() == '\n' && t.Last() != '\n')
            t.append(1, '\n');
        else if (orig.Last() != '\n' && t.Last() == '\n')
            t.RemoveLast();
    }

    return t;
}


/// Box sizer that allows one element to shrink below min size,
class ShrinkableBoxSizer : public wxBoxSizer
{
public:
    ShrinkableBoxSizer(int orient) : wxBoxSizer(orient) {}

    void SetShrinkableWindow(wxWindow *win)
    {
        m_shrinkable = win ? GetItem(win) : nullptr;
    }

    void RecalcSizes() override
    {
        if (m_shrinkable)
        {
            const wxCoord totalSize = GetSizeInMajorDir(m_size);
            const wxCoord minSize = GetSizeInMajorDir(m_calculatedMinSize);
            // If there's not enough space, make shrinkable item proportional,
            // it will be resized under its minimal size then.
            m_shrinkable->SetProportion(totalSize < minSize ? 1 : 0);
        }

        wxBoxSizer::RecalcSizes();
    }

private:
    wxSizerItem *m_shrinkable;
};


} // anonymous namespace


/// Tag-like label, with background rounded rect
class EditingArea::TagLabel : public wxWindow
{
public:
    enum Mode
    {
        Fixed,
        Ellipsize
    };

    TagLabel(wxWindow *parent, Color fg, Color bg) : wxWindow(parent, wxID_ANY)
    {
        m_fg = ColorScheme::Get(fg);
        m_bg = ColorScheme::Get(bg);

        m_label = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        m_label->SetForegroundColour(m_fg);
#ifdef __WXOSX__
        m_label->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
#endif
#ifdef __WXMSW__
        SetBackgroundColour(parent->GetBackgroundColour());
        m_label->SetBackgroundColour(ColorScheme::GetBlendedOn(bg, this));
#endif

        auto sizer = new wxBoxSizer(wxHORIZONTAL);
        sizer->Add(m_label, wxSizerFlags(1).Center().Border(wxALL, PX(2)));
#ifdef __WXMSW__
        sizer->InsertSpacer(0, PX(2));
        sizer->AddSpacer(PX(2));
#endif
        SetSizer(sizer);

        Bind(wxEVT_PAINT, &TagLabel::OnPaint, this);
    }

    void SetLabel(const wxString& text) override
    {
        m_label->SetLabel(text);
        InvalidateBestSize();
    }

protected:
    void DoSetToolTipText(const wxString &tip) override
    {
        wxWindow::DoSetToolTipText(tip);
        m_label->SetToolTip(tip);
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        gc->SetBrush(m_bg);
        gc->SetPen(*wxTRANSPARENT_PEN);

        auto rect = GetClientRect();
        gc->DrawRoundedRectangle(rect.x, rect.y, rect.width, rect.height, PX(2));
    }

    wxColour m_fg;
    wxBrush m_bg;
    wxStaticText *m_label;
};


EditingArea::EditingArea(wxWindow *parent, PoeditListCtrl *associatedList, MainToolbar *associatedToolbar, Mode mode)
    : m_associatedList(associatedList),
      m_associatedToolbar(associatedToolbar),
      m_dontAutoclearFuzzyStatus(false),
      m_textOrig(nullptr),
      m_textOrigPlural(nullptr),
      m_textTrans(nullptr),
      m_pluralNotebook(nullptr),
      m_labelSingular(nullptr),
      m_labelPlural(nullptr),
      m_labelSource(nullptr),
      m_labelTrans(nullptr),
      m_tagContext(nullptr),
      m_tagFormat(nullptr),
      m_errorBar(nullptr)
{
    wxPanel::Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                    wxTAB_TRAVERSAL | wxNO_BORDER | wxFULL_REPAINT_ON_RESIZE);
#ifdef __WXMSW__
    SetDoubleBuffered(true);
#endif

    SetBackgroundColour(ColorScheme::Get(Color::EditingBackground));
    Bind(wxEVT_PAINT, &EditingArea::OnPaint, this);

    m_labelSource = new wxStaticText(this, -1, _("Source text:"));
#ifdef __WXOSX__
    m_labelSource->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
#endif
    m_labelSource->SetFont(m_labelSource->GetFont().Bold());

    m_tagContext = new TagLabel(this, Color::TagContextFg, Color::TagContextBg);
    m_tagFormat = new TagLabel(this, Color::TagFormatFg, Color::TagFormatBg);

    auto sourceLineSizer = new ShrinkableBoxSizer(wxHORIZONTAL);
    sourceLineSizer->Add(m_labelSource, wxSizerFlags().Center().Border(wxBOTTOM, MACOS_OR_OTHER(2, 0)));
    sourceLineSizer->AddSpacer(PX(4));
    sourceLineSizer->Add(m_tagContext, wxSizerFlags(1).Center().Border(wxRIGHT, PX(6)));
    sourceLineSizer->Add(m_tagFormat, wxSizerFlags().Center().Border(wxRIGHT, PX(6)));
    sourceLineSizer->SetShrinkableWindow(m_tagContext);
    sourceLineSizer->SetMinSize(-1, m_tagContext->GetSize().y);

    m_labelSingular = new wxStaticText(this, -1, _("Singular:"));
    m_labelSingular->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
    m_labelSingular->SetFont(m_labelSingular->GetFont().Bold());
    m_labelSingular->SetForegroundColour(ColorScheme::Get(Color::SecondaryLabel));
    m_textOrig = new SourceTextCtrl(this, wxID_ANY);

    m_labelPlural = new wxStaticText(this, -1, _("Plural:"));
    m_labelPlural->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
    m_labelPlural->SetFont(m_labelPlural->GetFont().Bold());
    m_labelPlural->SetForegroundColour(ColorScheme::Get(Color::SecondaryLabel));
    m_textOrigPlural = new SourceTextCtrl(this, wxID_ANY);

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(sizer);

#if defined(__WXMSW__)
    sizer->AddSpacer(PX(4) - 4); // account for fixed 4px sash above
#elif defined(__WXOSX__)
    sizer->AddSpacer(PX(2));
#endif
    sizer->Add(sourceLineSizer, wxSizerFlags().Expand().Border(wxLEFT, PX(6)));
    sizer->AddSpacer(PX(6));

    sizer->Add(m_labelSingular, wxSizerFlags().Border(wxLEFT|wxTOP, PX(6)));
    sizer->Add(m_textOrig, wxSizerFlags(1).Expand().Border(wxLEFT|wxRIGHT, PX(4)));
    sizer->Add(m_labelPlural, wxSizerFlags().Border(wxLEFT, PX(6)));
    sizer->Add(m_textOrigPlural, wxSizerFlags(1).Expand().Border(wxLEFT|wxRIGHT, PX(4)));

    if (mode == POT)
        CreateTemplateControls(sizer);
    else
        CreateEditControls(sizer);
}


void EditingArea::CreateEditControls(wxBoxSizer *sizer)
{
    m_labelTrans = new wxStaticText(this, -1, _("Translation:"));
#ifdef __WXOSX__
    m_labelTrans->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
#endif
    m_labelTrans->SetFont(m_labelTrans->GetFont().Bold());

    m_textTrans = new TranslationTextCtrl(this, wxID_ANY);
    m_textTrans->Bind(wxEVT_TEXT, [=](wxCommandEvent& e){ e.Skip(); UpdateFromTextCtrl(); });

    // in case of plurals form, this is the control for n=1:
    m_textTransSingularForm = nullptr;

    m_pluralNotebook = new wxNotebook(this, -1, wxDefaultPosition, wxDefaultSize, wxNB_NOPAGETHEME);
    m_pluralNotebook->SetWindowVariant(wxWINDOW_VARIANT_SMALL);

    m_errorBar = new ErrorBar(this);

    sizer->Add(m_labelTrans, wxSizerFlags().Expand().Border(wxLEFT|wxTOP, PX(6)));
    sizer->AddSpacer(PX(6));
    sizer->Add(m_textTrans, wxSizerFlags(1).Expand().Border(wxLEFT|wxRIGHT|wxBOTTOM, PX(4)));
    sizer->Add(m_pluralNotebook, wxSizerFlags(3).Expand().Border(wxTOP, PX(4)));
    sizer->Add(m_errorBar, wxSizerFlags().Border(wxALL, PX(4)));

    ShowPluralFormUI(false);
}


void EditingArea::CreateTemplateControls(wxBoxSizer *panelSizer)
{
    auto win = new wxPanel(this, wxID_ANY);
    auto sizer = new wxBoxSizer(wxVERTICAL);

    auto explain = new wxStaticText(win, wxID_ANY, _(L"POT files are only templates and don’t contain any translations themselves.\nTo make a translation, create a new PO file based on the template."), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
#ifdef __WXOSX__
    explain->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
#endif
    explain->SetForegroundColour(ExplanationLabel::GetTextColor().ChangeLightness(160));
    win->SetBackgroundColour(GetBackgroundColour().ChangeLightness(50));

    auto button = new wxButton(win, XRCID("button_new_from_this_pot"), MSW_OR_OTHER(_("Create new translation"), _("Create New Translation")));

    sizer->AddStretchSpacer();
    sizer->Add(explain, wxSizerFlags().Center().Border(wxLEFT|wxRIGHT, PX(100)));
    sizer->Add(button, wxSizerFlags().Center().Border(wxTOP|wxBOTTOM, PX(10)));
    sizer->AddStretchSpacer();

    win->SetSizerAndFit(sizer);

    panelSizer->Add(win, 1, wxEXPAND);
}


EditingArea::~EditingArea()
{
    // OnPaint may still be called as child windows are destroyed
    m_labelSource = m_labelTrans = nullptr;
}


void EditingArea::OnPaint(wxPaintEvent&)
{
    wxPaintDC dc(this);
    auto width = GetClientSize().x;

    auto clr = ColorScheme::Get(Color::EditingSeparator);
    dc.SetPen(clr);
    dc.SetBrush(clr);

    const int paddingTop = MACOS_OR_OTHER(PX(2), PX(4));
    const int paddingBottom = PX(5);

    if (m_labelSource)
    {
        dc.DrawRectangle(0, m_labelSource->GetPosition().y + m_labelSource->GetSize().y + paddingBottom, width, PX(1));
    }

    if (m_labelTrans)
    {
        dc.DrawRectangle(0, m_labelTrans->GetPosition().y - paddingTop, width, PX(1));
        dc.DrawRectangle(0, m_labelTrans->GetPosition().y + m_labelTrans->GetSize().y + paddingBottom, width, PX(1));
    }
}



void EditingArea::SetCustomFont(const wxFont& font)
{
    SetCtrlFont(m_textOrig, font);
    SetCtrlFont(m_textOrigPlural, font);
    SetCtrlFont(m_textTrans, font);
    for (auto tp : m_textTransPlural)
        SetCtrlFont(tp, font);
}


bool EditingArea::InitSpellchecker(bool enabled, Language lang)
{
    bool rv = true;

    if (m_textTrans)
    {
        if (!InitTextCtrlSpellchecker(m_textTrans, enabled, lang))
            rv = false;
    }

    for (auto tp : m_textTransPlural)
    {
        if (tp && !InitTextCtrlSpellchecker(tp, enabled, lang))
            rv = false;
    }

    return rv;
}


void EditingArea::SetLanguage(Language lang)
{
    if (m_textTrans)
        m_textTrans->SetLanguage(lang);

    for (auto tp : m_textTransPlural)
    {
        if (tp)
            tp->SetLanguage(lang);
    }
}


void EditingArea::RecreatePluralTextCtrls(CatalogPtr catalog)
{
    if (!m_pluralNotebook)
        return;

    m_textTransPlural.clear();
    m_pluralNotebook->DeleteAllPages();
    m_textTransSingularForm = NULL;

    auto calc = PluralFormsCalculator::make(catalog->Header().GetHeader("Plural-Forms").ToAscii());

    int formsCount = catalog->GetPluralFormsCount();
    for (int form = 0; form < formsCount; form++)
    {
        // find example number that would use this plural form:
        static const int maxExamplesCnt = 5;
        wxString examples;
        int firstExample = -1;
        int examplesCnt = 0;

        if (calc && formsCount > 1)
        {
            for (int example = 0; example < 1000; example++)
            {
                if (calc->evaluate(example) == form)
                {
                    if (++examplesCnt == 1)
                        firstExample = example;
                    if (examplesCnt == maxExamplesCnt)
                    {
                        examples += L'…';
                        break;
                    }
                    else if (examplesCnt == 1)
                        examples += wxString::Format("%d", example);
                    else
                        examples += wxString::Format(", %d", example);
                }
            }
        }

        wxString desc;
        if (formsCount == 1)
            desc = _("Everything");
        else if (examplesCnt == 0)
            desc.Printf(_("Form %i"), form);
        else if (examplesCnt == 1)
        {
            if (formsCount == 2 && firstExample == 1) // English-like
            {
                desc = _("Singular");
            }
            else
            {
                if (firstExample == 0)
                    desc = _("Zero");
                else if (firstExample == 1)
                    desc = _("One");
                else if (firstExample == 2)
                    desc = _("Two");
                else
                    desc.Printf(L"n = %s", examples);
            }
        }
        else if (formsCount == 2 && examplesCnt == 2 && firstExample == 0 && examples == "0, 1")
        {
            desc = _("Singular");
        }
        else if (formsCount == 2 && firstExample != 1 && examplesCnt == maxExamplesCnt)
        {
            if (firstExample == 0 || firstExample == 2)
                desc = _("Plural");
            else
                desc = _("Other");
        }
        else
            desc.Printf(L"n → %s", examples);

        // create text control and notebook page for it:
        auto txt = new TranslationTextCtrl(m_pluralNotebook, wxID_ANY);
        txt->SetWindowVariant(wxWINDOW_VARIANT_NORMAL);
#ifndef __WXOSX__
        txt->SetFont(m_textTrans->GetFont());
#endif
        txt->Bind(wxEVT_TEXT, [=](wxCommandEvent& e){ e.Skip(); UpdateFromTextCtrl(); });
        m_textTransPlural.push_back(txt);
        m_pluralNotebook->AddPage(txt, desc);

        if (examplesCnt == 1 && firstExample == 1) // == singular
            m_textTransSingularForm = txt;
    }

    // as a fallback, assume 1st form for plural entries is the singular
    // (like in English and most real-life uses):
    if (!m_textTransSingularForm && !m_textTransPlural.empty())
        m_textTransSingularForm = m_textTransPlural[0];
}


void EditingArea::ShowPluralFormUI(bool show)
{
    wxSizer *origSizer = m_textOrig->GetContainingSizer();
    origSizer->Show(m_labelSingular, show);
    origSizer->Show(m_labelPlural, show);
    origSizer->Show(m_textOrigPlural, show);
    origSizer->Layout();

    if (m_textTrans && m_pluralNotebook)
    {
        wxSizer *textSizer = m_textTrans->GetContainingSizer();
        textSizer->Show(m_textTrans, !show);
        textSizer->Show(m_pluralNotebook, show);
        textSizer->Layout();
    }
}


void EditingArea::ShowPart(wxWindow *part, bool show)
{
    part->GetContainingSizer()->Show(part, show);
}


void EditingArea::SetSingleSelectionMode()
{
    if (!IsThisEnabled())
        Enable();  // in case of previous multiple selection
}


void EditingArea::SetMultipleSelectionMode()
{
    // TODO: Show better UI
    Disable();
}


void EditingArea::SetTextFocus()
{
    if (m_textTrans && m_textTrans->IsShown())
        m_textTrans->SetFocus();
    else if (!m_textTransPlural.empty())
        m_textTransPlural[0]->SetFocus();
}

bool EditingArea::HasTextFocus()
{
    wxWindow *focus = wxWindow::FindFocus();
    return (focus == m_textTrans) ||
           (focus && focus->GetParent() == m_pluralNotebook);
}

bool EditingArea::HasTextFocusInPlurals()
{
    if (!m_pluralNotebook || !m_pluralNotebook->IsShown())
        return false;

    auto focused = dynamic_cast<TranslationTextCtrl*>(FindFocus());
    if (!focused)
        return false;

    return std::find(m_textTransPlural.begin(), m_textTransPlural.end(), focused) != m_textTransPlural.end();
}


void EditingArea::CopyFromSingular()
{
    auto current = dynamic_cast<TranslationTextCtrl*>(wxWindow::FindFocus());
    if (!current || !m_textTransSingularForm)
        return;

    current->SetPlainTextUserWritten(m_textTransSingularForm->GetPlainText());
}


void EditingArea::UpdateToTextCtrl(CatalogItemPtr item, int flags)
{
    auto syntax = SyntaxHighlighter::ForItem(*item);
    m_textOrig->SetSyntaxHighlighter(syntax);
    if (m_textTrans)
        m_textTrans->SetSyntaxHighlighter(syntax);
    if (item->HasPlural())
    {
        m_textOrigPlural->SetSyntaxHighlighter(syntax);
        for (auto p : m_textTransPlural)
            p->SetSyntaxHighlighter(syntax);
    }

    m_textOrig->SetPlainText(item->GetString());

    if (item->HasPlural())
    {
        m_textOrigPlural->SetPlainText(item->GetPluralString());

        unsigned formsCnt = (unsigned)m_textTransPlural.size();
        for (unsigned j = 0; j < formsCnt; j++)
            SetTranslationValue(m_textTransPlural[j], wxEmptyString, flags);

        unsigned i = 0;
        for (i = 0; i < std::min(formsCnt, item->GetNumberOfTranslations()); i++)
        {
            SetTranslationValue(m_textTransPlural[i], item->GetTranslation(i), flags);
        }
    }
    else
    {
        if (m_textTrans)
            SetTranslationValue(m_textTrans, item->GetTranslation(), flags);
    }

    ShowPart(m_tagContext, item->HasContext());
    if (item->HasContext())
    {
        m_tagContext->SetLabel(item->GetContext());
        m_tagContext->SetToolTip(item->GetContext());
    }

    auto format = item->GetFormatFlag();
    ShowPart(m_tagFormat, !format.empty());
    if (!format.empty())
    {
        // TRANSLATORS: %s is replaced with language name, e.g. "PHP" or "C", so "PHP Format" etc."
        m_tagFormat->SetLabel(wxString::Format(MSW_OR_OTHER(_("%s format"), _("%s Format")), format.Upper()));
    }

    if (m_errorBar)
    {
        if (item->GetValidity() == CatalogItem::Val_Invalid)
            m_errorBar->ShowError(item->GetErrorString());
        else
            m_errorBar->HideError();
    }

    ShowPluralFormUI(item->HasPlural());

    Layout();

    Refresh();

    // by default, editing fuzzy item unfuzzies it
    m_dontAutoclearFuzzyStatus = false;
}


void EditingArea::UpdateFromTextCtrl()
{
    auto item = m_associatedList->GetCurrentCatalogItem();
    if (!item)
        return;

    wxString key = item->GetString();
    bool newfuzzy = m_associatedToolbar->IsFuzzy();

    const bool oldIsTranslated = item->IsTranslated();
    bool allTranslated = true; // will be updated later
    bool anyTransChanged = false; // ditto

    if (item->HasPlural())
    {
        wxArrayString str;
        for (unsigned i = 0; i < m_textTransPlural.size(); i++)
        {
            auto val = PreprocessEnteredTextForItem(item, m_textTransPlural[i]->GetPlainText());
            str.Add(val);
            if ( val.empty() )
                allTranslated = false;
        }

        if ( str != item->GetTranslations() )
        {
            anyTransChanged = true;
            item->SetTranslations(str);
        }
    }
    else
    {
        auto newval = PreprocessEnteredTextForItem(item, m_textTrans->GetPlainText());

        if ( newval.empty() )
            allTranslated = false;

        if ( newval != item->GetTranslation() )
        {
            anyTransChanged = true;
            item->SetTranslation(newval);
        }
    }

    if (item->IsFuzzy() == newfuzzy && !anyTransChanged)
    {
        return; // not even fuzzy status changed, so return
    }

    // did something affecting statistics change?
    bool statisticsChanged = false;

    if (newfuzzy == item->IsFuzzy() && !m_dontAutoclearFuzzyStatus)
        newfuzzy = false;

    m_associatedToolbar->SetFuzzy(newfuzzy);

    if ( item->IsFuzzy() != newfuzzy )
    {
        item->SetFuzzy(newfuzzy);
        statisticsChanged = true;
    }
    if ( oldIsTranslated != allTranslated )
    {
        item->SetTranslated(allTranslated);
        statisticsChanged = true;
    }
    item->SetModified(true);
    item->SetPreTranslated(false);

    m_associatedList->RefreshSelectedItems();

    if (OnUpdatedFromTextCtrl)
        OnUpdatedFromTextCtrl(item, statisticsChanged);
}


void EditingArea::ChangeFocusedPluralTab(int offset)
{
    wxCHECK_RET(offset == +1 || offset == -1, "invalid offset");

    m_pluralNotebook->AdvanceSelection(/*forward=*/offset == +1 ? true : false);
    m_textTransPlural[m_pluralNotebook->GetSelection()]->SetFocus();
}
