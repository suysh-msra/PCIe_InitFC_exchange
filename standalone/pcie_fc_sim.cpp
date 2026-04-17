// ============================================================================
// PCIe Flow Control Initialization & TLP Traffic Simulator
//
// Models two devices (Root Complex + Endpoint) connected by a PCIe link.
// Output is a two-column "lifeline" ASCII art diagram showing:
//   - DLLP arrows for FC Init handshake
//   - TLP header boxes with spec-compliant field encodings
//   - Credit gauge bars with fill indicators
//   - Back-pressure (BLOCKED) and credit-return (UpdateFC) events
//
// All output goes to both stdout (with ANSI color) and pcie_sim.log (plain).
// ============================================================================

#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <queue>
#include <vector>
#include <string>
#include <cstdint>
#include <cassert>

// ============================================================================
//  ANSI color codes (terminal only; stripped from log file)
// ============================================================================
static const char* RST = "\033[0m";
static const char* RED = "\033[31m";
static const char* GRN = "\033[32m";
static const char* YLW = "\033[33m";
static const char* BLU = "\033[34m";
static const char* MAG = "\033[35m";
static const char* CYN = "\033[36m";
static const char* BLD = "\033[1m";
static const char* DIM = "\033[2m";

// ============================================================================
//  Enumerations
// ============================================================================

enum class FCGroup : int { Posted = 0, NonPosted = 1, Completion = 2 };
static const int NUM_FC_GROUPS = 3;

const char* fc_group_str(FCGroup g) {
    switch (g) {
        case FCGroup::Posted:     return "Posted";
        case FCGroup::NonPosted:  return "NonPosted";
        case FCGroup::Completion: return "Completion";
    }
    return "?";
}

const char* fc_group_short(FCGroup g) {
    switch (g) {
        case FCGroup::Posted:     return "P";
        case FCGroup::NonPosted:  return "NP";
        case FCGroup::Completion: return "Cpl";
    }
    return "?";
}

enum class FCInitState { FC_INIT1, FC_INIT2, FC_READY };

const char* fc_state_str(FCInitState s) {
    switch (s) {
        case FCInitState::FC_INIT1: return "FC_INIT1";
        case FCInitState::FC_INIT2: return "FC_INIT2";
        case FCInitState::FC_READY: return "FC_READY";
    }
    return "?";
}

enum class DLLPType {
    InitFC1_P, InitFC1_NP, InitFC1_Cpl,
    InitFC2_P, InitFC2_NP, InitFC2_Cpl,
    UpdateFC_P, UpdateFC_NP, UpdateFC_Cpl
};

const char* dllp_type_str(DLLPType t) {
    switch (t) {
        case DLLPType::InitFC1_P:    return "InitFC1-P";
        case DLLPType::InitFC1_NP:   return "InitFC1-NP";
        case DLLPType::InitFC1_Cpl:  return "InitFC1-Cpl";
        case DLLPType::InitFC2_P:    return "InitFC2-P";
        case DLLPType::InitFC2_NP:   return "InitFC2-NP";
        case DLLPType::InitFC2_Cpl:  return "InitFC2-Cpl";
        case DLLPType::UpdateFC_P:   return "UpdateFC-P";
        case DLLPType::UpdateFC_NP:  return "UpdateFC-NP";
        case DLLPType::UpdateFC_Cpl: return "UpdateFC-Cpl";
    }
    return "?";
}

FCGroup dllp_to_fc_group(DLLPType t) {
    switch (t) {
        case DLLPType::InitFC1_P:   case DLLPType::InitFC2_P:   case DLLPType::UpdateFC_P:   return FCGroup::Posted;
        case DLLPType::InitFC1_NP:  case DLLPType::InitFC2_NP:  case DLLPType::UpdateFC_NP:  return FCGroup::NonPosted;
        case DLLPType::InitFC1_Cpl: case DLLPType::InitFC2_Cpl: case DLLPType::UpdateFC_Cpl: return FCGroup::Completion;
    }
    return FCGroup::Posted;
}

enum TLPFmt : uint8_t {
    FMT_3DW_NO_DATA = 0b00,
    FMT_4DW_NO_DATA = 0b01,
    FMT_3DW_DATA    = 0b10,
    FMT_4DW_DATA    = 0b11
};

enum TLPTypeBits : uint8_t {
    TLPTYPE_MEM  = 0b00000,
    TLPTYPE_CPL  = 0b01010,
};

enum class TLPCategory { MemRead32, MemWrite32, CplD, Cpl };

const char* tlp_cat_str(TLPCategory c) {
    switch (c) {
        case TLPCategory::MemRead32:  return "MRd32";
        case TLPCategory::MemWrite32: return "MWr32";
        case TLPCategory::CplD:       return "CplD";
        case TLPCategory::Cpl:        return "Cpl";
    }
    return "?";
}

// ============================================================================
//  Data Structures
// ============================================================================

struct DLLP {
    DLLPType type;
    uint8_t  hdr_credits;
    uint16_t data_credits;
};

struct TLPHeader {
    uint8_t  fmt;
    uint8_t  type;
    uint8_t  tc;
    bool     td;
    bool     ep;
    uint8_t  attr;
    uint16_t length;
    uint16_t requester_id;
    uint8_t  tag;
    uint8_t  last_dw_be;
    uint8_t  first_dw_be;
    uint32_t address;
    uint16_t completer_id;
    uint8_t  cpl_status;
    uint16_t byte_count;
    uint8_t  lower_address;
};

struct TLP {
    TLPCategory           category;
    TLPHeader             header;
    std::vector<uint32_t> data;
    uint16_t              seq_num;

    FCGroup get_fc_group() const {
        switch (category) {
            case TLPCategory::MemWrite32: return FCGroup::Posted;
            case TLPCategory::MemRead32:  return FCGroup::NonPosted;
            case TLPCategory::CplD:
            case TLPCategory::Cpl:        return FCGroup::Completion;
        }
        return FCGroup::Posted;
    }
    bool has_data() const { return (header.fmt & 0b10) != 0; }
    uint16_t data_credit_cost() const {
        if (!has_data() || header.length == 0) return 0;
        return (header.length + 3) / 4;
    }
};

// Credit tracker: one per FC group on the TX side.
// The RECEIVER advertises credits.  The SENDER records that as credit_limit
// and increments credits_consumed with each TLP sent.  Blocked when consumed == limit.
// Advertised value 0 = infinite (never blocks).
struct CreditTracker {
    uint8_t  hdr_limit     = 0;
    uint16_t data_limit    = 0;
    uint8_t  hdr_consumed  = 0;
    uint16_t data_consumed = 0;
    bool     infinite_hdr  = false;
    bool     infinite_data = false;

    void set_limit(uint8_t h, uint16_t d) {
        if (h == 0) infinite_hdr = true; else hdr_limit = h;
        if (d == 0) infinite_data = true; else data_limit = d;
    }
    bool has_credits(uint8_t hdr_need, uint16_t data_need) const {
        bool hdr_ok  = infinite_hdr  || (uint8_t)(hdr_limit - hdr_consumed) >= hdr_need;
        bool data_ok = infinite_data || (uint16_t)(data_limit - data_consumed) >= data_need;
        return hdr_ok && data_ok;
    }
    void consume(uint8_t h, uint16_t d) {
        if (!infinite_hdr)  hdr_consumed  += h;
        if (!infinite_data) data_consumed += d;
    }
    void update_limit(uint8_t h, uint16_t d) {
        if (!infinite_hdr)  hdr_limit  = h;
        if (!infinite_data) data_limit = d;
    }

    int hdr_avail() const {
        if (infinite_hdr) return -1;
        return (uint8_t)(hdr_limit - hdr_consumed);
    }
    int data_avail() const {
        if (infinite_data) return -1;
        return (uint16_t)(data_limit - data_consumed);
    }
};

// ============================================================================
//  Hex formatting helper
// ============================================================================
static std::string hex32(uint32_t v) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << v;
    return ss.str();
}
static std::string hex16(uint16_t v) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << v;
    return ss.str();
}

// ============================================================================
//  Diagram Renderer
//
//  Draws a two-column "lifeline" diagram.  All text is built as plain ASCII,
//  then emitted to both stdout (with optional ANSI color) and the log file.
//
//  Layout constants:
//    LCOL = 10  (left lifeline '|' column)
//    RCOL = 78  (right lifeline '|' column)
//    IW   = 67  (inner width between lifelines, exclusive of '|' chars)
//    Line width = 79
// ============================================================================

class Diagram {
public:
    static const int LCOL = 10;
    static const int RCOL = 78;
    static const int IW   = RCOL - LCOL - 1;   // 67
    static const int LW   = RCOL + 1;           // 79, full line width

    Diagram(const std::string& filename) : file_(filename) {
        if (!file_.is_open()) {
            std::cerr << "Warning: could not open " << filename << " for writing\n";
        }
    }

    // ------ primitives ------

    void emit(const std::string& line, const char* color = "") {
        if (color[0]) std::cout << color << line << RST << "\n";
        else          std::cout << line << "\n";
        if (file_.is_open()) file_ << line << "\n";
    }

    // Lifeline line:  "          |{inner padded to IW}|"
    std::string ll(const std::string& inner = "") const {
        std::string s(LCOL, ' ');
        s += '|';
        s += inner;
        int pad = IW - static_cast<int>(inner.size());
        if (pad > 0) s.append(pad, ' ');
        s += '|';
        return s;
    }

    void blank() { emit(ll()); }

    // ------ structural elements ------

    void device_header(const std::string& left_name, const std::string& left_bdf,
                       const std::string& left_buf,
                       const std::string& right_name, const std::string& right_bdf,
                       const std::string& right_buf) {
        // Top boxes
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

        // Bottom with lifeline drop
        std::string bot = " +" + std::string((box_w - 1) / 2, '-') + "+"
                        + std::string(box_w / 2, '-') + "+";
        emit(bot + gap + bot, BLD);

        blank();
    }

    void cycle_sep(int cycle) {
        std::string label = " CYCLE " + std::to_string(cycle) + " ";
        std::string s(LW, '=');
        s[LCOL] = '+';
        s[RCOL] = '+';
        int start = (LW - static_cast<int>(label.size())) / 2;
        s.replace(start, label.size(), label);
        emit(s, BLD);
    }

    void phase_banner(const std::string& text) {
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

    // ------ DLLP arrows (grouped) ------

    void dllp_group_right(const std::vector<DLLP>& dllps) {
        for (auto& d : dllps) {
            std::string name = dllp_type_str(d.type);
            name.resize(12, ' ');
            std::string info = "  " + name + "[HdrCr="
                + std::to_string(d.hdr_credits) + " DataCr="
                + std::to_string(d.data_credits) + "]";
            if (d.hdr_credits == 0 && d.data_credits == 0)
                info += " (infinite)";
            emit(ll(info), GRN);
        }
        std::string arrow(IW, '-');
        arrow[IW - 1] = ' ';
        arrow[IW - 2] = '>';
        arrow[IW - 3] = '>';
        emit(ll(arrow), GRN);
    }

    void dllp_group_left(const std::vector<DLLP>& dllps) {
        for (auto& d : dllps) {
            std::string name = dllp_type_str(d.type);
            name.resize(12, ' ');
            std::string info = name + "[HdrCr="
                + std::to_string(d.hdr_credits) + " DataCr="
                + std::to_string(d.data_credits) + "]";
            // right-align
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

    void dllp_single_right(const DLLP& d) {
        dllp_group_right({d});
    }
    void dllp_single_left(const DLLP& d) {
        dllp_group_left({d});
    }

    // ------ state transitions ------

    void state_left(FCInitState from, FCInitState to, const std::string& reason) {
        std::string line = "  * " + std::string(fc_state_str(from))
            + " --> " + std::string(fc_state_str(to)) + " *";
        emit(ll(line), YLW);
        emit(ll("    (" + reason + ")"), DIM);
    }

    void state_right(FCInitState from, FCInitState to, const std::string& reason) {
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

    // ------ DL_Up banner ------

    void dl_up() {
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

    // ------ TLP header box ------

    void tlp_box_right(const TLP& t, const CreditTracker& cred) {
        render_tlp_box(t, cred, true);
    }
    void tlp_box_left(const TLP& t, const CreditTracker& cred) {
        render_tlp_box(t, cred, false);
    }

    // ------ BLOCKED ------

    void blocked_right(const TLP& t, const CreditTracker& cred) {
        // (blocked on right device -- rarely used in our sim)
        blocked_impl(t, cred, false);
    }
    void blocked_left(const TLP& t, const CreditTracker& cred) {
        blocked_impl(t, cred, true);
    }

    // ------ Notes ------

    void note_left(const std::string& text) {
        emit(ll("  " + text), DIM);
    }
    void note_right(const std::string& text) {
        int pad = IW - static_cast<int>(text.size()) - 2;
        std::string inner(pad > 0 ? pad : 0, ' ');
        inner += text;
        emit(ll(inner), DIM);
    }
    void note_center(const std::string& text) {
        int pad_l = (IW - static_cast<int>(text.size())) / 2;
        std::string inner(IW, ' ');
        inner.replace(pad_l, text.size(), text);
        emit(ll(inner), DIM);
    }

    // ------ Credit gauges ------

    void credit_gauge(const std::string& label, const CreditTracker creds[3]) {
        static const char* names[] = { "P  ", "NP ", "Cpl" };
        int bw = 50;  // box outer width
        int bi = bw - 4;  // content width (minus "| " and " |")

        auto pad_str = [](const std::string& s, int w) {
            std::string r = s;
            if (static_cast<int>(r.size()) < w) r.append(w - r.size(), ' ');
            return r;
        };

        std::string margin(2, ' ');

        // Top
        std::string top = margin + "+" + std::string(2, '-') + " "
            + label + " " + std::string(bw - 6 - static_cast<int>(label.size()), '-') + "+";
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

            std::string content = pad_str(oss.str(), bi);
            emit(ll(margin + "| " + content + " |"), BLU);
        }

        // Bottom
        emit(ll(margin + "+" + std::string(bw - 2, '-') + "+"), BLU);
    }

private:
    std::ofstream file_;

    std::string gauge_bar(const CreditTracker& c) const {
        const int bar_len = 12;
        if (c.infinite_hdr && c.infinite_data) {
            return "[ infinite  ]";
        }
        // Use header utilization for the bar (usually the bottleneck)
        int used = c.infinite_hdr ? 0 : (int)c.hdr_consumed;
        int total = c.infinite_hdr ? 1 : (int)c.hdr_limit;
        int filled = (total > 0) ? (used * bar_len / total) : 0;
        if (filled > bar_len) filled = bar_len;
        std::string bar(filled, '#');
        bar.append(bar_len - filled, '.');
        return "[" + bar + "]";
    }

    // Build TLP header box lines
    std::vector<std::string> build_tlp_box_lines(const TLP& t) const {
        std::vector<std::string> lines;
        std::ostringstream oss;

        // Title
        oss << tlp_cat_str(t.category) << "  Seq#" << t.seq_num
            << "  [" << fc_group_short(t.get_fc_group()) << "]";
        lines.push_back(oss.str()); oss.str("");

        // DW0: build Type[4:0] as a 5-bit binary string
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

        // Data payload (show first 4 DW)
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

    void render_tlp_box(const TLP& t, const CreditTracker& cred, bool left_to_right) {
        auto box_lines = build_tlp_box_lines(t);

        // Find max content width
        int max_content = 0;
        for (auto& l : box_lines)
            if (static_cast<int>(l.size()) > max_content) max_content = l.size();
        int box_inner = max_content + 2;  // 1 space padding each side
        int box_outer = box_inner + 2;    // + 2 for '|' chars

        int margin = left_to_right ? 2 : (IW - 2 - box_outer);
        if (margin < 1) margin = 1;
        std::string mg(margin, ' ');

        auto pad_to = [](const std::string& s, int w) {
            std::string r = s;
            if (static_cast<int>(r.size()) < w) r.append(w - r.size(), ' ');
            return r;
        };

        // Title line with top border
        std::string title = box_lines[0];
        std::string top = mg + "+-- " + title + " "
            + std::string(box_outer - 5 - static_cast<int>(title.size()), '-') + "+";
        emit(ll(top), left_to_right ? GRN : CYN);

        // Content lines
        for (size_t i = 1; i < box_lines.size(); i++) {
            std::string row = mg + "| " + pad_to(box_lines[i], box_inner - 2) + " |";
            emit(ll(row), left_to_right ? GRN : CYN);
        }

        // Bottom border
        emit(ll(mg + "+" + std::string(box_outer - 2, '-') + "+"),
             left_to_right ? GRN : CYN);

        // Thick arrow with credit info
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
        emit(ll(arrow), left_to_right ? GRN : CYN);
    }

    void blocked_impl(const TLP& t, const CreditTracker& cred, bool on_left) {
        // X-pattern line
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
};

// ============================================================================
//  PCIe Device (silent — no direct output; all rendering via Diagram)
// ============================================================================

class PCIeDevice {
public:
    std::string name_;
    uint16_t    bdf_;
    FCInitState fc_state_ = FCInitState::FC_INIT1;

    uint8_t  rx_hdr_cap_[NUM_FC_GROUPS]  = {};
    uint16_t rx_data_cap_[NUM_FC_GROUPS] = {};
    CreditTracker tx_cred_[NUM_FC_GROUPS];

    bool initfc1_sent_[NUM_FC_GROUPS]     = {};
    bool initfc1_received_[NUM_FC_GROUPS] = {};
    bool initfc2_sent_[NUM_FC_GROUPS]     = {};
    bool initfc2_received_ = false;

    std::queue<DLLP> tx_dllp_q_;
    std::queue<DLLP> rx_dllp_q_;
    std::queue<TLP>  tx_tlp_q_;
    std::queue<TLP>  rx_tlp_q_;

    uint16_t next_seq_ = 0;
    uint8_t  next_tag_ = 0;

    PCIeDevice(const std::string& name, uint16_t bdf,
               uint8_t ph, uint16_t pd, uint8_t nph, uint16_t npd,
               uint8_t cplh, uint16_t cpld)
        : name_(name), bdf_(bdf)
    {
        rx_hdr_cap_[(int)FCGroup::Posted]      = ph;
        rx_data_cap_[(int)FCGroup::Posted]     = pd;
        rx_hdr_cap_[(int)FCGroup::NonPosted]   = nph;
        rx_data_cap_[(int)FCGroup::NonPosted]  = npd;
        rx_hdr_cap_[(int)FCGroup::Completion]  = cplh;
        rx_data_cap_[(int)FCGroup::Completion] = cpld;
    }

    bool is_dl_up() const { return fc_state_ == FCInitState::FC_READY; }

    // Run one FC init tick (process RX DLLPs, generate TX DLLPs, check transitions)
    void fc_init_tick() {
        drain_rx_dllps();
        switch (fc_state_) {
        case FCInitState::FC_INIT1:
            emit_initfc1();
            if (initfc1_received_[0] && initfc1_received_[1] && initfc1_received_[2])
                fc_state_ = FCInitState::FC_INIT2;
            break;
        case FCInitState::FC_INIT2:
            emit_initfc2();
            if (initfc2_received_)
                fc_state_ = FCInitState::FC_READY;
            break;
        case FCInitState::FC_READY:
            break;
        }
    }

    // Extract all pending TX DLLPs (for rendering + manual transfer)
    std::vector<DLLP> take_tx_dllps() {
        std::vector<DLLP> out;
        while (!tx_dllp_q_.empty()) {
            out.push_back(tx_dllp_q_.front());
            tx_dllp_q_.pop();
        }
        return out;
    }

    // TLP construction
    void enqueue_mem_write(uint32_t addr, const std::vector<uint32_t>& payload) {
        TLP tlp{};
        tlp.category            = TLPCategory::MemWrite32;
        tlp.header.fmt          = FMT_3DW_DATA;
        tlp.header.type         = TLPTYPE_MEM;
        tlp.header.tc           = 0;
        tlp.header.length       = static_cast<uint16_t>(payload.size());
        tlp.header.requester_id = bdf_;
        tlp.header.tag          = next_tag_++;
        tlp.header.first_dw_be  = 0xF;
        tlp.header.last_dw_be   = (payload.size() > 1) ? 0xF : 0x0;
        tlp.header.address      = addr & 0xFFFFFFFC;
        tlp.data                = payload;
        tlp.seq_num             = next_seq_++;
        tx_tlp_q_.push(tlp);
    }

    void enqueue_mem_read(uint32_t addr, uint16_t length_dw) {
        TLP tlp{};
        tlp.category            = TLPCategory::MemRead32;
        tlp.header.fmt          = FMT_3DW_NO_DATA;
        tlp.header.type         = TLPTYPE_MEM;
        tlp.header.tc           = 0;
        tlp.header.length       = length_dw;
        tlp.header.requester_id = bdf_;
        tlp.header.tag          = next_tag_++;
        tlp.header.first_dw_be  = 0xF;
        tlp.header.last_dw_be   = (length_dw > 1) ? 0xF : 0x0;
        tlp.header.address      = addr & 0xFFFFFFFC;
        tlp.seq_num             = next_seq_++;
        tx_tlp_q_.push(tlp);
    }

    void enqueue_completion(uint16_t req_id, uint8_t tag, uint16_t byte_count,
                            const std::vector<uint32_t>& payload, uint32_t lower_addr) {
        TLP tlp{};
        tlp.category            = payload.empty() ? TLPCategory::Cpl : TLPCategory::CplD;
        tlp.header.fmt          = payload.empty() ? FMT_3DW_NO_DATA : FMT_3DW_DATA;
        tlp.header.type         = TLPTYPE_CPL;
        tlp.header.tc           = 0;
        tlp.header.length       = static_cast<uint16_t>(payload.size());
        tlp.header.completer_id = bdf_;
        tlp.header.requester_id = req_id;
        tlp.header.tag          = tag;
        tlp.header.cpl_status   = 0;
        tlp.header.byte_count   = byte_count;
        tlp.header.lower_address = static_cast<uint8_t>(lower_addr & 0x7F);
        tlp.data                = payload;
        tlp.seq_num             = next_seq_++;
        tx_tlp_q_.push(tlp);
    }

    // Try to transmit the front TLP; returns true and fills 'out' on success.
    bool try_transmit_tlp(TLP& out) {
        if (tx_tlp_q_.empty()) return false;
        const TLP& front = tx_tlp_q_.front();
        int gi = (int)front.get_fc_group();
        if (!tx_cred_[gi].has_credits(1, front.data_credit_cost()))
            return false;
        tx_cred_[gi].consume(1, front.data_credit_cost());
        out = front;
        tx_tlp_q_.pop();
        return true;
    }

    bool has_pending_tlp() const { return !tx_tlp_q_.empty(); }
    const TLP& peek_pending_tlp() const { return tx_tlp_q_.front(); }

    void receive_tlp(const TLP& tlp) { rx_tlp_q_.push(tlp); }

    // Process RX TLPs — generates completions for reads.
    // Returns descriptions of what happened (for rendering).
    struct RxAction {
        std::string description;
        bool generated_completion;
    };
    std::vector<RxAction> process_rx_tlps() {
        std::vector<RxAction> actions;
        while (!rx_tlp_q_.empty()) {
            TLP tlp = rx_tlp_q_.front();
            rx_tlp_q_.pop();
            switch (tlp.category) {
            case TLPCategory::MemRead32: {
                uint16_t bc = tlp.header.length * 4;
                std::vector<uint32_t> rdata(tlp.header.length);
                for (size_t i = 0; i < rdata.size(); i++)
                    rdata[i] = 0xCAFE0000 | static_cast<uint32_t>(i);
                enqueue_completion(tlp.header.requester_id, tlp.header.tag,
                                   bc, rdata, tlp.header.address & 0x7F);
                actions.push_back({"Gen CplD for Tag=" + std::to_string(tlp.header.tag)
                    + " BC=" + std::to_string(bc), true});
                break;
            }
            case TLPCategory::MemWrite32: {
                std::ostringstream oss;
                oss << "Absorbed MWr: " << tlp.header.length << "DW @ 0x"
                    << hex32(tlp.header.address);
                actions.push_back({oss.str(), false});
                break;
            }
            case TLPCategory::CplD:
            case TLPCategory::Cpl:
                actions.push_back({"Cpl consumed: Tag=" + std::to_string(tlp.header.tag)
                    + " BC=" + std::to_string(tlp.header.byte_count), false});
                break;
            }
        }
        return actions;
    }

    // Manually inject an UpdateFC DLLP into the TX queue
    void send_update_fc(FCGroup grp, uint8_t hdr_cr, uint16_t data_cr) {
        DLLPType dtype;
        switch (grp) {
            case FCGroup::Posted:     dtype = DLLPType::UpdateFC_P;   break;
            case FCGroup::NonPosted:  dtype = DLLPType::UpdateFC_NP;  break;
            case FCGroup::Completion: dtype = DLLPType::UpdateFC_Cpl; break;
        }
        tx_dllp_q_.push({dtype, hdr_cr, data_cr});
    }

private:
    void drain_rx_dllps() {
        while (!rx_dllp_q_.empty()) {
            DLLP dllp = rx_dllp_q_.front();
            rx_dllp_q_.pop();
            int gi = (int)dllp_to_fc_group(dllp.type);
            switch (dllp.type) {
            case DLLPType::InitFC1_P: case DLLPType::InitFC1_NP: case DLLPType::InitFC1_Cpl:
                initfc1_received_[gi] = true;
                tx_cred_[gi].set_limit(dllp.hdr_credits, dllp.data_credits);
                break;
            case DLLPType::InitFC2_P: case DLLPType::InitFC2_NP: case DLLPType::InitFC2_Cpl:
                initfc2_received_ = true;
                break;
            case DLLPType::UpdateFC_P: case DLLPType::UpdateFC_NP: case DLLPType::UpdateFC_Cpl:
                tx_cred_[gi].update_limit(dllp.hdr_credits, dllp.data_credits);
                break;
            }
        }
    }
    void emit_initfc1() {
        static const DLLPType types[] = {DLLPType::InitFC1_P, DLLPType::InitFC1_NP, DLLPType::InitFC1_Cpl};
        for (int i = 0; i < NUM_FC_GROUPS; i++) {
            if (!initfc1_sent_[i]) {
                tx_dllp_q_.push({types[i], rx_hdr_cap_[i], rx_data_cap_[i]});
                initfc1_sent_[i] = true;
            }
        }
    }
    void emit_initfc2() {
        static const DLLPType types[] = {DLLPType::InitFC2_P, DLLPType::InitFC2_NP, DLLPType::InitFC2_Cpl};
        for (int i = 0; i < NUM_FC_GROUPS; i++) {
            if (!initfc2_sent_[i]) {
                tx_dllp_q_.push({types[i], rx_hdr_cap_[i], rx_data_cap_[i]});
                initfc2_sent_[i] = true;
            }
        }
    }
};

// ============================================================================
//  Helper: transfer DLLPs between devices via the Diagram (render + deliver)
// ============================================================================
static void transfer_and_render_dllps(PCIeDevice& left, PCIeDevice& right, Diagram& d) {
    auto left_dllps  = left.take_tx_dllps();
    auto right_dllps = right.take_tx_dllps();

    if (!left_dllps.empty()) {
        d.dllp_group_right(left_dllps);
        for (auto& dl : left_dllps) right.rx_dllp_q_.push(dl);
    }
    if (!right_dllps.empty()) {
        if (!left_dllps.empty()) d.blank();
        d.dllp_group_left(right_dllps);
        for (auto& dl : right_dllps) left.rx_dllp_q_.push(dl);
    }
}

// ============================================================================
//  Main Simulation
// ============================================================================

int main() {
    Diagram dia("pcie_sim.log");

    //                          name           BDF     PH  PD  NPH NPD CplH CplD
    PCIeDevice rc("RootComplex", 0x0100,        8,  32,   4,  16,   0,   0);
    PCIeDevice ep("Endpoint",    0x0200,        4,   8,   2,   4,   4,   8);

    // ================================================================
    //  Device Header
    // ================================================================
    dia.device_header(
        "RootComplex", "01:00.0", "PH=8 NPH=4 CplH=INF",
        "Endpoint",    "02:00.0", "PH=4 NPH=2 CplH=4");

    // ================================================================
    //  PHASE 1 — FC Initialization Handshake
    // ================================================================
    dia.phase_banner("PHASE 1:  FC Initialization Handshake (InitFC1 / InitFC2)");

    // Cycle 0: both emit InitFC1
    dia.cycle_sep(0);
    dia.blank();

    FCInitState rc_pre = rc.fc_state_, ep_pre = ep.fc_state_;
    rc.fc_init_tick();
    ep.fc_init_tick();
    transfer_and_render_dllps(rc, ep, dia);
    dia.blank();

    // Cycle 1: both receive InitFC1, transition FC_INIT1 -> FC_INIT2
    dia.cycle_sep(1);
    dia.blank();

    rc_pre = rc.fc_state_; ep_pre = ep.fc_state_;
    rc.fc_init_tick();
    if (rc.fc_state_ != rc_pre)
        dia.state_left(rc_pre, rc.fc_state_, "all 3 InitFC1 groups received");
    ep.fc_init_tick();
    if (ep.fc_state_ != ep_pre)
        dia.state_right(ep_pre, ep.fc_state_, "all 3 InitFC1 groups received");
    transfer_and_render_dllps(rc, ep, dia);  // no DLLPs this cycle
    dia.blank();

    // Cycle 2: both emit InitFC2
    dia.cycle_sep(2);
    dia.blank();

    rc_pre = rc.fc_state_; ep_pre = ep.fc_state_;
    rc.fc_init_tick();
    ep.fc_init_tick();
    transfer_and_render_dllps(rc, ep, dia);
    dia.blank();

    // Cycle 3: both receive InitFC2, transition FC_INIT2 -> FC_READY
    dia.cycle_sep(3);
    dia.blank();

    rc_pre = rc.fc_state_; ep_pre = ep.fc_state_;
    rc.fc_init_tick();
    if (rc.fc_state_ != rc_pre)
        dia.state_left(rc_pre, rc.fc_state_, "InitFC2 received - DL_Up!");
    ep.fc_init_tick();
    if (ep.fc_state_ != ep_pre)
        dia.state_right(ep_pre, ep.fc_state_, "InitFC2 received - DL_Up!");
    transfer_and_render_dllps(rc, ep, dia);

    dia.dl_up();

    dia.note_center("Credit State After FC Init:");
    dia.credit_gauge("RC TX Credits (limit = EP's buffers)", rc.tx_cred_);
    dia.blank();
    dia.credit_gauge("EP TX Credits (limit = RC's buffers)", ep.tx_cred_);
    dia.blank();

    // ================================================================
    //  PHASE 2 — Posted Writes (MWr32, RC -> EP)
    // ================================================================
    dia.phase_banner("PHASE 2:  Memory Writes  (RC --> EP,  Posted)");

    // Write 1: 4 DW to 0x1000
    dia.cycle_sep(4);
    dia.blank();

    rc.enqueue_mem_write(0x0000'1000, {0xDEAD'BEEF, 0x1234'5678, 0xA5A5'A5A5, 0xFEED'FACE});
    {
        TLP tlp;
        if (rc.try_transmit_tlp(tlp)) {
            dia.tlp_box_right(tlp, rc.tx_cred_[(int)tlp.get_fc_group()]);
            ep.receive_tlp(tlp);
            auto actions = ep.process_rx_tlps();
            for (auto& a : actions) dia.note_right(a.description);
        }
    }
    dia.blank();

    // Write 2: 2 DW to 0x2000
    dia.cycle_sep(5);
    dia.blank();

    rc.enqueue_mem_write(0x0000'2000, {0x1111'1111, 0x2222'2222});
    {
        TLP tlp;
        if (rc.try_transmit_tlp(tlp)) {
            dia.tlp_box_right(tlp, rc.tx_cred_[(int)tlp.get_fc_group()]);
            ep.receive_tlp(tlp);
            auto actions = ep.process_rx_tlps();
            for (auto& a : actions) dia.note_right(a.description);
        }
    }
    dia.blank();

    dia.note_center("Credit State After Writes:");
    dia.credit_gauge("RC TX Credits", rc.tx_cred_);
    dia.blank();

    // ================================================================
    //  PHASE 3 — Non-Posted Read + Completion (MRd32 -> CplD)
    // ================================================================
    dia.phase_banner("PHASE 3:  Memory Read + Completion  (RC --> EP --> RC)");

    // MRd: RC -> EP
    dia.cycle_sep(6);
    dia.blank();

    rc.enqueue_mem_read(0x0000'3000, 4);
    {
        TLP tlp;
        if (rc.try_transmit_tlp(tlp)) {
            dia.tlp_box_right(tlp, rc.tx_cred_[(int)tlp.get_fc_group()]);
            ep.receive_tlp(tlp);
            auto actions = ep.process_rx_tlps();
            for (auto& a : actions) dia.note_right(a.description);
        }
    }
    dia.blank();

    // CplD: EP -> RC
    dia.cycle_sep(7);
    dia.blank();

    {
        TLP tlp;
        if (ep.try_transmit_tlp(tlp)) {
            dia.tlp_box_left(tlp, ep.tx_cred_[(int)tlp.get_fc_group()]);
            rc.receive_tlp(tlp);
            auto actions = rc.process_rx_tlps();
            for (auto& a : actions) dia.note_left(a.description);
        }
    }
    dia.blank();

    dia.note_center("Credit State After Read+Completion:");
    dia.credit_gauge("RC TX Credits", rc.tx_cred_);
    dia.blank();
    dia.credit_gauge("EP TX Credits", ep.tx_cred_);
    dia.blank();

    // ================================================================
    //  PHASE 4 — Credit Exhaustion
    //  EP advertised PH=4.  RC already consumed 2 PH credits.
    //  2 more MWr32 will succeed, then BLOCKED.
    // ================================================================
    dia.phase_banner("PHASE 4:  Credit Exhaustion  (back-pressure)");
    dia.note_left("RC has consumed 2 of 4 Posted header credits.");
    dia.note_left("Queuing 4 more MWr32 -- expect 2 OK, then BLOCKED.");
    dia.blank();

    for (int i = 0; i < 4; i++) {
        rc.enqueue_mem_write(0x0000'4000 + i * 0x100,
                             {static_cast<uint32_t>(0xBB00'0000 | i)});
    }

    int cycle = 8;
    for (int i = 0; i < 4; i++) {
        dia.cycle_sep(cycle++);
        dia.blank();

        TLP tlp;
        if (rc.try_transmit_tlp(tlp)) {
            dia.tlp_box_right(tlp, rc.tx_cred_[(int)tlp.get_fc_group()]);
            ep.receive_tlp(tlp);
            auto actions = ep.process_rx_tlps();
            for (auto& a : actions) dia.note_right(a.description);
        } else if (rc.has_pending_tlp()) {
            const TLP& blocked = rc.peek_pending_tlp();
            dia.blocked_left(blocked, rc.tx_cred_[(int)blocked.get_fc_group()]);
        }
        dia.blank();
    }

    dia.note_center("Credit State (exhausted):");
    dia.credit_gauge("RC TX Credits", rc.tx_cred_);
    dia.blank();

    // ================================================================
    //  PHASE 5 — Credit Return via UpdateFC
    // ================================================================
    dia.phase_banner("PHASE 5:  Credit Return via UpdateFC  (EP frees buffers)");

    // EP returns credits.  UpdateFC carries the new high-water mark.
    uint8_t  new_ph  = rc.tx_cred_[(int)FCGroup::Posted].hdr_consumed  + 4;
    uint16_t new_pd  = rc.tx_cred_[(int)FCGroup::Posted].data_consumed + 8;

    dia.cycle_sep(cycle++);
    dia.blank();

    dia.note_right("EP frees Posted buffers, sends UpdateFC");
    ep.send_update_fc(FCGroup::Posted, new_ph, new_pd);
    {
        auto ep_dllps = ep.take_tx_dllps();
        dia.dllp_group_left(ep_dllps);
        for (auto& d : ep_dllps) rc.rx_dllp_q_.push(d);
    }
    rc.fc_init_tick();   // drain the UpdateFC DLLP
    dia.blank();

    dia.note_center("Credit State After UpdateFC:");
    dia.credit_gauge("RC TX Credits", rc.tx_cred_);
    dia.blank();

    // Now retry the blocked TLPs
    dia.cycle_sep(cycle++);
    dia.blank();
    dia.note_left("RC retries previously-blocked MWr32 TLPs...");
    dia.blank();

    for (int i = 0; i < 2; i++) {
        TLP tlp;
        if (rc.try_transmit_tlp(tlp)) {
            dia.tlp_box_right(tlp, rc.tx_cred_[(int)tlp.get_fc_group()]);
            ep.receive_tlp(tlp);
            auto actions = ep.process_rx_tlps();
            for (auto& a : actions) dia.note_right(a.description);
            dia.blank();
        }
    }

    dia.note_center("Final Credit State:");
    dia.credit_gauge("RC TX Credits", rc.tx_cred_);
    dia.blank();
    dia.credit_gauge("EP TX Credits", ep.tx_cred_);
    dia.blank();

    // ================================================================
    //  Summary
    // ================================================================
    dia.phase_banner("SIMULATION COMPLETE");

    dia.note_center("Summary:");
    dia.note_center("Phase 1: FC Init (InitFC1 -> InitFC2 -> FC_READY)");
    dia.note_center("Phase 2: Posted MWr consumed EP's P credits");
    dia.note_center("Phase 3: Non-Posted MRd with CplD round-trip");
    dia.note_center("Phase 4: Credit exhaustion -> TX back-pressure");
    dia.note_center("Phase 5: UpdateFC restored credits, unblocked TX");
    dia.blank();
    dia.note_center("Full log saved to: pcie_sim.log");
    dia.blank();

    return 0;
}
