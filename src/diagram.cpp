#include "diagram.h"

#include <algorithm>
#include <iostream>
#include <sstream>

using namespace ansi;

// ============================================================================
//  Construction
// ============================================================================

Diagram::Diagram(const std::string& log_filename) : file_(log_filename) {
    if (!file_.is_open())
        std::cerr << "Warning: could not open " << log_filename << " for writing\n";
}

// ============================================================================
//  Primitives
// ============================================================================

void Diagram::emit(const std::string& line, const char* color) {
    if (color[0]) std::cout << color << line << RST << "\n";
    else          std::cout << line << "\n";
    if (file_.is_open()) file_ << line << "\n";
}

std::string Diagram::ll(const std::string& inner) const {
    std::string s(LCOL, ' ');
    s += '|';
    s += inner;
    int pad = IW - static_cast<int>(inner.size());
    if (pad > 0) s.append(pad, ' ');
    s += '|';
    return s;
}

void Diagram::blank() { emit(ll()); }

// ============================================================================
//  Structural elements
// ============================================================================

void Diagram::device_header(const std::string& left_name, const std::string& left_bdf,
                            const std::string& left_buf,
                            const std::string& right_name, const std::string& right_bdf,
                            const std::string& right_buf) {
    int box_w = 20;
    std::string top = " +" + std::string(box_w, '-') + "+";
    std::string gap(IW - 2 * box_w - 2, ' ');
    emit(top + gap + top, BLD);

    auto center = [&](const std::string& text, int w) {
        int pad_l = (w - static_cast<int>(text.size())) / 2;
        int pad_r = w - static_cast<int>(text.size()) - pad_l;
        return std::string(pad_l, ' ') + text + std::string(pad_r, ' ');
    };

    auto row = [&](const std::string& l, const std::string& r) {
        emit(" |" + center(l, box_w) + "|" + gap + "|" + center(r, box_w) + "|", BLD);
    };
    row(left_name, right_name);
    row("(" + left_bdf + ")", "(" + right_bdf + ")");
    emit(" |" + std::string(box_w, ' ') + "|"
         + center("PCIe Link", static_cast<int>(gap.size()))
         + "|" + std::string(box_w, ' ') + "|", DIM);
    row(left_buf, right_buf);

    std::string bot = " +" + std::string((box_w - 1) / 2, '-') + "+"
                    + std::string(box_w / 2, '-') + "+";
    emit(bot + gap + bot, BLD);

    blank();
}

void Diagram::cycle_sep(int cycle) {
    std::string label = " CYCLE " + std::to_string(cycle) + " ";
    std::string s(LW, '=');
    s[LCOL] = '+';
    s[RCOL] = '+';
    int start = (LW - static_cast<int>(label.size())) / 2;
    s.replace(start, label.size(), label);
    emit(s, BLD);
}

void Diagram::phase_banner(const std::string& text) {
    emit("");
    std::string bar(LW, '#');
    emit(bar, MAG);
    int pad_l = (LW - static_cast<int>(text.size())) / 2;
    std::string line(LW, ' ');
    line[0] = '#'; line[1] = '#';
    line[LW - 2] = '#'; line[LW - 1] = '#';
    line.replace(pad_l, text.size(), text);
    emit(line, MAG);
    emit(bar, MAG);
    blank();
}

void Diagram::scenario_banner(const std::string& title, const std::string& subtitle) {
    emit("");
    emit("");
    std::string border(LW, '=');
    border[0] = '+'; border[LW - 1] = '+';
    emit(border, BLD);
    emit(border, BLD);

    auto center_line = [&](const std::string& text) {
        std::string row(LW, ' ');
        row[0] = '|'; row[1] = '|';
        row[LW - 2] = '|'; row[LW - 1] = '|';
        int pad_l = (LW - static_cast<int>(text.size())) / 2;
        row.replace(pad_l, text.size(), text);
        emit(row, BLD);
    };

    center_line(title);
    center_line(subtitle);

    emit(border, BLD);
    emit(border, BLD);
    emit("");
}

void Diagram::dl_up() {
    blank();
    std::string stars(IW, ' ');
    for (int i = 2; i < IW - 2; i += 2) stars[i] = '*';
    emit(ll(stars), BLD);
    std::string label = "DATA LINK IS UP";
    int pad_l = (IW - static_cast<int>(label.size())) / 2;
    std::string inner(IW, ' ');
    inner.replace(pad_l, label.size(), label);
    emit(ll(inner), BLD);
    emit(ll(stars), BLD);
    blank();
}

// ============================================================================
//  DLLP arrows
// ============================================================================

void Diagram::dllp_group_right(const std::vector<DLLP>& dllps,
                               bool show_vc, bool suppress_infinite) {
    for (auto& d : dllps) {
        std::string prefix;
        if (show_vc)
            prefix = "[VC" + std::to_string(d.vc) + "] ";

        std::string name = dllp_type_str(d.type);
        name.resize(12, ' ');
        std::string info = "  " + prefix + name + "[HdrCr="
            + std::to_string(d.hdr_credits) + " DataCr="
            + std::to_string(d.data_credits) + "]";
        if (!suppress_infinite && d.hdr_credits == 0 && d.data_credits == 0)
            info += " (infinite)";
        emit(ll(info), GRN);
    }
    std::string arrow(IW, '-');
    arrow[IW - 1] = ' ';
    arrow[IW - 2] = '>';
    arrow[IW - 3] = '>';
    emit(ll(arrow), GRN);
}

void Diagram::dllp_group_left(const std::vector<DLLP>& dllps,
                              bool show_vc, bool suppress_infinite) {
    for (auto& d : dllps) {
        std::string prefix;
        if (show_vc)
            prefix = "[VC" + std::to_string(d.vc) + "] ";

        std::string name = dllp_type_str(d.type);
        name.resize(12, ' ');
        std::string info = prefix + name + "[HdrCr="
            + std::to_string(d.hdr_credits) + " DataCr="
            + std::to_string(d.data_credits) + "]";
        if (!suppress_infinite && d.hdr_credits == 0 && d.data_credits == 0)
            info += " (infinite)";
        int pad = IW - static_cast<int>(info.size()) - 2;
        std::string inner(pad > 0 ? pad : 0, ' ');
        inner += info;
        emit(ll(inner), CYN);
    }
    std::string arrow(IW, '-');
    arrow[0] = ' ';
    arrow[1] = '<';
    arrow[2] = '<';
    emit(ll(arrow), CYN);
}

void Diagram::dllp_single_right(const DLLP& d) { dllp_group_right({d}); }
void Diagram::dllp_single_left(const DLLP& d)  { dllp_group_left({d}); }

// ============================================================================
//  Flit boxes (PCIe 6.0+ flit mode)
// ============================================================================

void Diagram::flit_box_right(const Flit& f) { render_flit_box(f, true); }
void Diagram::flit_box_left(const Flit& f)  { render_flit_box(f, false); }

void Diagram::render_flit_box(const Flit& f, bool left_to_right) {
    static const char* grp_names[] = {"Posted", "NonPosted", "Completion"};

    // Build content lines
    std::vector<std::string> lines;
    {
        std::ostringstream oss;
        oss << flit_type_str(f.type) << "  Seq#" << f.seq_num
            << "  [" << Flit::SIZE_BYTES << "B Flit]";
        lines.push_back(oss.str());
    }
    lines.push_back("All 3 FC groups in a single flit (no DLLPs)");
    for (int i = 0; i < NUM_FC_GROUPS; i++) {
        std::ostringstream oss;
        oss << grp_names[i] << ":";
        int pad = 12 - static_cast<int>(oss.str().size());
        if (pad > 0) oss << std::string(pad, ' ');
        oss << "HdrCr=" << (int)f.hdr_credits[i]
            << "  DataCr=" << (int)f.data_credits[i];
        if (f.hdr_credits[i] == 0 && f.data_credits[i] == 0)
            oss << "  (infinite)";
        lines.push_back(oss.str());
    }

    // Render box
    int max_content = 0;
    for (auto& l : lines)
        if (static_cast<int>(l.size()) > max_content) max_content = l.size();
    int box_inner = max_content + 2;
    int box_outer = box_inner + 2;

    int margin = left_to_right ? 2 : (IW - 2 - box_outer);
    if (margin < 1) margin = 1;
    std::string mg(margin, ' ');

    auto pad_to = [](const std::string& s, int w) {
        std::string r = s;
        if (static_cast<int>(r.size()) < w) r.append(w - r.size(), ' ');
        return r;
    };

    const char* color = left_to_right ? GRN : CYN;

    // Title line
    std::string title = lines[0];
    std::string top = mg + "+--- " + title + " "
        + std::string(box_outer - 6 - static_cast<int>(title.size()), '-') + "+";
    emit(ll(top), color);

    for (size_t i = 1; i < lines.size(); i++)
        emit(ll(mg + "| " + pad_to(lines[i], box_inner - 2) + " |"), color);

    emit(ll(mg + "+" + std::string(box_outer - 2, '-') + "+"), color);

    // Arrow
    std::string arrow(IW, '=');
    std::string label = "[256B FLIT]";
    int label_pos = (IW - static_cast<int>(label.size())) / 2;
    arrow.replace(label_pos, label.size(), label);

    if (left_to_right) {
        arrow[IW - 1] = ' ';
        arrow[IW - 2] = '>';
        arrow[IW - 3] = '>';
    } else {
        arrow[0] = ' ';
        arrow[1] = '<';
        arrow[2] = '<';
    }
    emit(ll(arrow), color);
}

// ============================================================================
//  State transitions
// ============================================================================

void Diagram::state_left(FCInitState from, FCInitState to, const std::string& reason) {
    std::string line = "  * " + std::string(fc_state_str(from))
        + " --> " + std::string(fc_state_str(to)) + " *";
    emit(ll(line), YLW);
    emit(ll("    (" + reason + ")"), DIM);
}

void Diagram::state_right(FCInitState from, FCInitState to, const std::string& reason) {
    std::string tag = "* " + std::string(fc_state_str(from))
        + " --> " + std::string(fc_state_str(to)) + " *";
    int pad = IW - static_cast<int>(tag.size()) - 2;
    std::string inner(pad > 0 ? pad : 0, ' ');
    inner += tag;
    emit(ll(inner), YLW);
    std::string r2 = "(" + reason + ")";
    int pad2 = IW - static_cast<int>(r2.size()) - 2;
    std::string inner2(pad2 > 0 ? pad2 : 0, ' ');
    inner2 += r2;
    emit(ll(inner2), DIM);
}

// ============================================================================
//  TLP header box
// ============================================================================

void Diagram::tlp_box_right(const TLP& t, const CreditTracker& cred) {
    render_tlp_box(t, cred, true);
}
void Diagram::tlp_box_left(const TLP& t, const CreditTracker& cred) {
    render_tlp_box(t, cred, false);
}

std::vector<std::string> Diagram::build_tlp_box_lines(const TLP& t) const {
    std::vector<std::string> lines;
    std::ostringstream oss;

    oss << tlp_cat_str(t.category) << "  Seq#" << t.seq_num
        << "  [" << fc_group_short(t.get_fc_group()) << "]";
    lines.push_back(oss.str()); oss.str("");

    char type_bits[6];
    for (int b = 4; b >= 0; b--)
        type_bits[4 - b] = ((t.header.type >> b) & 1) ? '1' : '0';
    type_bits[5] = '\0';

    oss << "DW0: Fmt=" << (int)t.header.fmt
        << " Type=" << type_bits
        << " TC=" << (int)t.header.tc
        << " Len=" << t.header.length << "DW";
    lines.push_back(oss.str()); oss.str("");

    if (t.category == TLPCategory::CplD || t.category == TLPCategory::Cpl) {
        oss << "DW1: CplrID=0x" << hex16(t.header.completer_id)
            << " Stat=" << (int)t.header.cpl_status
            << " BC=" << t.header.byte_count;
        lines.push_back(oss.str()); oss.str("");
        oss << "DW2: ReqID=0x" << hex16(t.header.requester_id)
            << " Tag=" << (int)t.header.tag
            << " LowAddr=0x" << std::hex << std::setfill('0')
            << std::setw(2) << (int)t.header.lower_address
            << std::dec << std::setfill(' ');
        lines.push_back(oss.str()); oss.str("");
    } else {
        oss << "DW1: ReqID=0x" << hex16(t.header.requester_id)
            << " Tag=" << (int)t.header.tag
            << " BE=" << std::hex << (int)t.header.first_dw_be
            << "h/" << (int)t.header.last_dw_be << "h" << std::dec;
        lines.push_back(oss.str()); oss.str("");
        oss << "DW2: Addr=0x" << hex32(t.header.address);
        lines.push_back(oss.str()); oss.str("");
    }

    if (t.has_data() && !t.data.empty()) {
        oss << "Data:";
        int show = std::min(static_cast<int>(t.data.size()), 4);
        for (int i = 0; i < show; i++)
            oss << " " << hex32(t.data[i]);
        if (static_cast<int>(t.data.size()) > 4) oss << " ...";
        lines.push_back(oss.str()); oss.str("");
    }
    return lines;
}

void Diagram::render_tlp_box(const TLP& t, const CreditTracker& cred, bool left_to_right) {
    auto box_lines = build_tlp_box_lines(t);

    int max_content = 0;
    for (auto& l : box_lines)
        if (static_cast<int>(l.size()) > max_content) max_content = l.size();
    int box_inner = max_content + 2;
    int box_outer = box_inner + 2;

    int margin = left_to_right ? 2 : (IW - 2 - box_outer);
    if (margin < 1) margin = 1;
    std::string mg(margin, ' ');

    auto pad_to = [](const std::string& s, int w) {
        std::string r = s;
        if (static_cast<int>(r.size()) < w) r.append(w - r.size(), ' ');
        return r;
    };

    const char* color = left_to_right ? GRN : CYN;

    std::string title = box_lines[0];
    std::string top = mg + "+-- " + title + " "
        + std::string(box_outer - 5 - static_cast<int>(title.size()), '-') + "+";
    emit(ll(top), color);

    for (size_t i = 1; i < box_lines.size(); i++) {
        std::string row = mg + "| " + pad_to(box_lines[i], box_inner - 2) + " |";
        emit(ll(row), color);
    }

    emit(ll(mg + "+" + std::string(box_outer - 2, '-') + "+"), color);

    std::string grp = fc_group_short(t.get_fc_group());
    std::ostringstream cl;
    cl << grp << ": H:";
    if (cred.infinite_hdr) cl << "INF";
    else cl << (int)cred.hdr_consumed << "/" << (int)cred.hdr_limit;
    cl << " D:";
    if (cred.infinite_data) cl << "INF";
    else cl << cred.data_consumed << "/" << cred.data_limit;
    std::string cred_label = "[" + cl.str() + "]";

    std::string arrow(IW, '=');
    int label_pos = (IW - static_cast<int>(cred_label.size())) / 2;
    arrow.replace(label_pos, cred_label.size(), cred_label);

    if (left_to_right) {
        arrow[IW - 1] = ' ';
        arrow[IW - 2] = '>';
        arrow[IW - 3] = '>';
    } else {
        arrow[0] = ' ';
        arrow[1] = '<';
        arrow[2] = '<';
    }
    emit(ll(arrow), color);
}

// ============================================================================
//  Blocked
// ============================================================================

void Diagram::blocked_left(const TLP& t, const CreditTracker& cred) {
    blocked_impl(t, cred, true);
}
void Diagram::blocked_right(const TLP& t, const CreditTracker& cred) {
    blocked_impl(t, cred, false);
}

void Diagram::blocked_impl(const TLP& t, const CreditTracker& cred, bool on_left) {
    std::string xs(IW, ' ');
    for (int i = (on_left ? 2 : IW/2); i < (on_left ? IW/2 : IW - 2); i += 2)
        xs[i] = 'X';
    emit(ll(xs), RED);

    std::string grp = fc_group_short(t.get_fc_group());
    std::ostringstream oss;
    oss << "BLOCKED: No " << fc_group_str(t.get_fc_group()) << " credits";
    std::string line1 = (on_left ? "  " : "") + oss.str();
    if (!on_left) {
        int pad = IW - static_cast<int>(line1.size()) - 2;
        line1 = std::string(pad > 0 ? pad : 0, ' ') + line1;
    }
    emit(ll(line1), RED);

    oss.str("");
    oss << tlp_cat_str(t.category) << " -> Addr=0x" << hex32(t.header.address)
        << " waiting...";
    std::string line2 = (on_left ? "  " : "") + oss.str();
    if (!on_left) {
        int pad = IW - static_cast<int>(line2.size()) - 2;
        line2 = std::string(pad > 0 ? pad : 0, ' ') + line2;
    }
    emit(ll(line2), RED);

    oss.str("");
    oss << "Have " << grp << "[H:";
    if (cred.infinite_hdr) oss << "INF";
    else oss << cred.hdr_avail();
    oss << " D:";
    if (cred.infinite_data) oss << "INF";
    else oss << cred.data_avail();
    oss << "]  Need H:1 D:" << t.data_credit_cost();
    std::string line3 = (on_left ? "  " : "") + oss.str();
    if (!on_left) {
        int pad = IW - static_cast<int>(line3.size()) - 2;
        line3 = std::string(pad > 0 ? pad : 0, ' ') + line3;
    }
    emit(ll(line3), RED);

    emit(ll(xs), RED);
}

// ============================================================================
//  Notes
// ============================================================================

void Diagram::note_left(const std::string& text) {
    emit(ll("  " + text), DIM);
}
void Diagram::note_right(const std::string& text) {
    int pad = IW - static_cast<int>(text.size()) - 2;
    std::string inner(pad > 0 ? pad : 0, ' ');
    inner += text;
    emit(ll(inner), DIM);
}
void Diagram::note_center(const std::string& text) {
    int pad_l = (IW - static_cast<int>(text.size())) / 2;
    std::string inner(IW, ' ');
    inner.replace(pad_l, text.size(), text);
    emit(ll(inner), DIM);
}

// ============================================================================
//  Credit gauges
// ============================================================================

std::string Diagram::gauge_bar(const CreditTracker& c) const {
    const int bar_len = 12;
    if (c.infinite_hdr && c.infinite_data)
        return "[ infinite  ]";
    int used  = c.infinite_hdr ? 0 : (int)c.hdr_consumed;
    int total = c.infinite_hdr ? 1 : (int)c.hdr_limit;
    int filled = (total > 0) ? (used * bar_len / total) : 0;
    if (filled > bar_len) filled = bar_len;
    std::string bar(filled, '#');
    bar.append(bar_len - filled, '.');
    return "[" + bar + "]";
}

void Diagram::credit_gauge(const std::string& label, const CreditTracker creds[3],
                            const char* annotations[3]) {
    static const char* names[] = { "P  ", "NP ", "Cpl" };
    int bw = 54;
    int bi = bw - 4;

    auto pad_str = [](const std::string& s, int w) {
        std::string r = s;
        if (static_cast<int>(r.size()) < w) r.append(w - r.size(), ' ');
        return r;
    };

    std::string margin(2, ' ');

    int label_space = bw - 6 - static_cast<int>(label.size());
    if (label_space < 1) label_space = 1;
    std::string top = margin + "+" + std::string(2, '-') + " "
        + label + " " + std::string(label_space, '-') + "+";
    emit(ll(top), BLU);

    for (int i = 0; i < NUM_FC_GROUPS; i++) {
        const auto& c = creds[i];
        std::string bar = gauge_bar(c);
        std::ostringstream oss;
        oss << "  " << names[i] << " " << bar << "  H:";
        if (c.infinite_hdr)  oss << "INF    ";
        else oss << std::setw(2) << (int)c.hdr_consumed << "/" << std::setw(2) << (int)c.hdr_limit << "  ";
        oss << "D:";
        if (c.infinite_data) oss << "INF";
        else oss << std::setw(2) << c.data_consumed << "/" << std::setw(2) << c.data_limit;
        if (annotations && annotations[i])
            oss << " " << annotations[i];

        std::string content = pad_str(oss.str(), bi);
        emit(ll(margin + "| " + content + " |"), BLU);
    }

    emit(ll(margin + "+" + std::string(bw - 2, '-') + "+"), BLU);
}
